/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "hydra/file_reader.h"
#include "hydra/camera.h"
#include "hydra/render_delegate.h"
#include "hydra/session.h"

#include "util/log.h"
#include "util/path.h"
#include "util/unique_ptr.h"

#include "scene/scene.h"

#include <pxr/base/plug/registry.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>

/* Scene index task prims need USD 25.02, older versions use the USD imaging delegate. */
#if __has_include(<pxr/imaging/hd/legacyTaskFactory.h>)
#  define WITH_HYDRA_SCENE_INDEX_TASKS
#  include <pxr/imaging/hd/flatteningSceneIndex.h>
#  include <pxr/imaging/hd/legacyTaskFactory.h>
#  include <pxr/imaging/hd/legacyTaskSchema.h>
#  include <pxr/imaging/hd/mergingSceneIndex.h>
#  include <pxr/imaging/hd/retainedDataSource.h>
#  include <pxr/imaging/hd/retainedSceneIndex.h>
#  include <pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h>
#  include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#  include <pxr/usdImaging/usdImaging/flattenedDataSourceProviders.h>
#  include <pxr/usdImaging/usdImaging/sceneIndices.h>
#  include <pxr/usdImaging/usdImaging/stageSceneIndex.h>
#else
#  include <pxr/usdImaging/usdImaging/delegate.h>
#endif

HDCYCLES_NAMESPACE_OPEN_SCOPE

/* Dummy task whose only purpose is to provide render tag tokens to the render index. */
class DummyHdTask : public HdTask {
 public:
  DummyHdTask(HdSceneDelegate * /*delegate*/, const SdfPath &id)
      : HdTask(id), tags({HdRenderTagTokens->geometry, HdRenderTagTokens->render})
  {
  }

 protected:
  void Sync(HdSceneDelegate * /*delegate*/,
            HdTaskContext * /*ctx*/,
            HdDirtyBits * /*dirtyBits*/) override
  {
  }

  void Prepare(HdTaskContext * /*ctx*/, HdRenderIndex * /*render_index*/) override {}

  void Execute(HdTaskContext * /*ctx*/) override {}

  const TfTokenVector &GetRenderTags() const override
  {
    return tags;
  }

  TfTokenVector tags;
};

void HdCyclesFileReader::read(Session *session, const char *filepath, const bool use_camera)
{
  /* Initialize USD. */
  PlugRegistry::GetInstance().RegisterPlugins(path_get("usd"));

  /* Open Stage. */
  const UsdStageRefPtr stage = UsdStage::Open(filepath);
  if (!stage) {
    LOG_ERROR << "USD failed to read " << filepath;
    return;
  }

  read(session, stage, UsdTimeCode::Default(), SdfPath(), use_camera);
}

void HdCyclesFileReader::read(Session *session,
                              const UsdStageRefPtr &stage,
                              const UsdTimeCode time,
                              const SdfPath &camera_path,
                              const bool use_camera,
                              const CameraUtilConformWindowPolicy window_policy)
{
  HdCyclesFileReader reader(session, stage, UsdGeomGetStageMetersPerUnit(stage), true);
  reader.sync(time, camera_path, use_camera, window_policy);
}

struct HdCyclesFileReader::State {
  Session *session = nullptr;
  UsdStageRefPtr stage;
  const SdfPath task_path = SdfPath("/_hdCycles/DummyHdTask");
  HdRprimCollection collection;

  /* Destroyed in reverse order: the scene before the render index, the render index before the
   * render delegate. */
  unique_ptr<HdCyclesDelegate> render_delegate;
  unique_ptr<HdRenderIndex> render_index;
#ifdef WITH_HYDRA_SCENE_INDEX_TASKS
  UsdImagingStageSceneIndexRefPtr stage_si;
#else
  unique_ptr<UsdImagingDelegate> scene_delegate;
#endif
};

HdCyclesFileReader::HdCyclesFileReader(Session *session,
                                       const UsdStageRefPtr &stage,
                                       const double meters_per_unit,
                                       const bool keep_nodes,
                                       const std::vector<float> &motion_times)
    : state_(make_unique<State>())
{
  State &state = *state_;
  state.session = session;
  state.stage = stage;

  /* Init paths. */
  const SdfPath root_path = SdfPath::AbsoluteRootPath();

  /* Create render delegate. */
  HdRenderSettingsMap settings_map;
  settings_map.insert(
      std::make_pair(HdCyclesRenderSettingsTokens->stageMetersPerUnit, VtValue(meters_per_unit)));

  state.render_delegate = make_unique<HdCyclesDelegate>(settings_map, session, keep_nodes);
  static_cast<HdCyclesSession *>(state.render_delegate->GetRenderParam())->motion_times =
      motion_times;

  /* Create render index. */
  state.render_index.reset(HdRenderIndex::New(state.render_delegate.get(), {}));

  state.collection = HdRprimCollection(HdTokens->geometry,
                                       HdReprSelector(HdReprTokens->smoothHull));
  state.collection.SetRootPath(root_path);

#ifdef WITH_HYDRA_SCENE_INDEX_TASKS
  /* Set up scene index from USD stage for easy consumption: the scene indices of USD imaging
   * (instancing, material bindings, ...), then ext computation (e.g. skinning), implicit
   * surfaces as meshes and flattening. */
  UsdImagingCreateSceneIndicesInfo create_info;
  create_info.stage = stage;
  const UsdImagingSceneIndices scene_indices = UsdImagingCreateSceneIndices(create_info);
  state.stage_si = scene_indices.stageSceneIndex;

  HdSceneIndexBaseRefPtr filtered = scene_indices.finalSceneIndex;
  filtered = HdSiExtComputationPrimvarPruningSceneIndex::New(filtered);
  /* Cycles has no implicit surfaces, turn USD Sphere, Cube, ... into meshes. */
  const HdDataSourceBaseHandle to_mesh = HdRetainedTypedSampledDataSource<TfToken>::New(
      HdsiImplicitSurfaceSceneIndexTokens->toMesh);
  const TfToken implicit_types[] = {HdPrimTypeTokens->sphere,
                                    HdPrimTypeTokens->cube,
                                    HdPrimTypeTokens->cone,
                                    HdPrimTypeTokens->cylinder,
                                    HdPrimTypeTokens->capsule,
                                    HdPrimTypeTokens->plane};
  const HdDataSourceBaseHandle implicit_modes[] = {
      to_mesh, to_mesh, to_mesh, to_mesh, to_mesh, to_mesh};
  filtered = HdsiImplicitSurfaceSceneIndex::New(
      filtered,
      HdRetainedContainerDataSource::New(
          std::size(implicit_types), implicit_types, implicit_modes));
  filtered = HdFlatteningSceneIndex::New(filtered, UsdImagingFlattenedDataSourceProviders());

  /* Provide DummyHdTask as a task prim to the scene index. */
  const TfTokenVector render_tags = {HdRenderTagTokens->geometry, HdRenderTagTokens->render};

  HdContainerDataSourceHandle task_ds =
      HdLegacyTaskSchema::Builder()
          .SetFactory(HdRetainedTypedSampledDataSource<HdLegacyTaskFactorySharedPtr>::New(
              HdMakeLegacyTaskFactory<DummyHdTask>()))
          .SetCollection(
              HdRetainedTypedSampledDataSource<HdRprimCollection>::New(state.collection))
          .SetRenderTags(HdRetainedTypedSampledDataSource<TfTokenVector>::New(render_tags))
          .Build();

  HdRetainedSceneIndexRefPtr task_si = HdRetainedSceneIndex::New();
  task_si->AddPrims(
      {{state.task_path,
        HdPrimTypeTokens->task,
        HdRetainedContainerDataSource::New(HdLegacyTaskSchema::GetSchemaToken(), task_ds)}});

  HdMergingSceneIndexRefPtr merging_si = HdMergingSceneIndex::New();
  merging_si->AddInputScene(filtered, root_path);
  merging_si->AddInputScene(task_si, root_path);

  const bool needs_prefixing = false;
  state.render_index->InsertSceneIndex(merging_si, root_path, needs_prefixing);
#else
  /* Create scene delegate, add render tags and create prims. */
  state.scene_delegate = make_unique<UsdImagingDelegate>(state.render_index.get(), root_path);
  state.render_index->InsertTask<DummyHdTask>(state.scene_delegate.get(), state.task_path);
  state.scene_delegate->Populate(stage->GetPseudoRoot());
#endif
}

HdCyclesFileReader::~HdCyclesFileReader() = default;

bool HdCyclesFileReader::sync(const UsdTimeCode time,
                              const SdfPath &camera_path,
                              const bool use_camera,
                              const CameraUtilConformWindowPolicy window_policy,
                              SdfPath *synced_camera)
{
  State &state = *state_;

  /* Apply the stage changes since the last sync. */
#ifdef WITH_HYDRA_SCENE_INDEX_TASKS
  state.stage_si->SetTime(time);
  state.stage_si->ApplyPendingUpdates();
#else
  /* USD imaging samples motion over the shutter of its sampling camera. */
  if (static_cast<HdCyclesSession *>(state.render_delegate->GetRenderParam())
          ->motion_times.size() > 1)
  {
    for (const UsdPrim &prim : state.stage->Traverse()) {
      if (prim.IsA<UsdGeomCamera>() && (camera_path.IsEmpty() || prim.GetPath() == camera_path)) {
        state.scene_delegate->SetCameraForSampling(prim.GetPath());
        break;
      }
    }
  }
  state.scene_delegate->ApplyPendingUpdates();
  state.scene_delegate->SetTime(time);
#endif

  /* Sync prims. */
  state.render_index->EnqueueCollectionToSync(state.collection);

  HdTaskContext task_context;
  HdTaskSharedPtrVector tasks;
  if (HdTaskSharedPtr const &task = state.render_index->GetTask(state.task_path)) {
    tasks.push_back(task);
  }

  state.render_index->SyncAll(&tasks, &task_context);
  state.render_delegate->CommitResources(&state.render_index->GetChangeTracker());

  if (!use_camera) {
    return true;
  }

  /* Use first camera in stage.
   * TODO: get camera from UsdRender if available. */
  for (const UsdPrim &prim : state.stage->Traverse()) {
    if (prim.IsA<UsdGeomCamera>() && (camera_path.IsEmpty() || prim.GetPath() == camera_path)) {
      HdSprim *sprim = state.render_index->GetSprim(HdPrimTypeTokens->camera, prim.GetPath());
      if (sprim) {
        HdCyclesCamera *camera = dynamic_cast<HdCyclesCamera *>(sprim);
        camera->SetWindowPolicy(window_policy);
        camera->ApplyCameraSettings(state.render_delegate->GetRenderParam(),
                                    state.session->scene->camera);
        if (synced_camera) {
          *synced_camera = prim.GetPath();
        }
        return true;
      }
    }
  }
  return false;
}

HDCYCLES_NAMESPACE_CLOSE_SCOPE
