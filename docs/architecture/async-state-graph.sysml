package BasicRendererAsyncStateArchitecture {
    item def SourceRevision;
    item def ArtifactRequest;
    item def ArtifactVersion;
    item def GpuSubmission;
    item def PublicationFragment;
    item def FrameManifest;
    item def PublishedStateLease;

    port def RequestPort { out item request : ArtifactRequest; }
    port def ArtifactPort { out item artifact : ArtifactVersion; }
    port def SubmissionPort { out item submission : GpuSubmission; }
    port def FragmentPort { out item fragment : PublicationFragment; }
    port def ManifestPort { out item manifest : FrameManifest; }
    port def StateLeasePort { out item publishedLease : PublishedStateLease; }
    port def DependencyPort { out item artifactVersion : ArtifactVersion; }

    /* Spec42 draws interconnection edges between ports. Specialize this for
       logical graph nodes that both consume and produce dependency state. */
    part def DependencyNode {
        in port dependencyInput : ~DependencyPort;
        out port dependencyOutput : DependencyPort;
    }

    part def RequestSources {
        part scene : SceneRequests;
        part materials : MaterialRequests;
        part textures : TextureRequests;
        part geometry : GeometryRequests;
        part objects : ObjectRequests;
        part viewsLightsPoses : ViewLightPoseRequests;
        part terrain : TerrainRequests;
        part staticScene : StaticSceneRequests;

        out port requests : RequestPort;
    }

    part def SceneRequests;
    part def MaterialRequests;
    part def TextureRequests;
    part def GeometryRequests;
    part def ObjectRequests;
    part def ViewLightPoseRequests;
    part def TerrainRequests;
    part def StaticSceneRequests;

    part def AsyncStateGraphKernel {
        part admission : RequestAdmission;
        part versionStore : VersionStore;
        part dependencyResolver : DependencyResolver;
        part scheduler : ProducerScheduler;
        part readinessBroker : ReadinessBroker;
        part lifetimeManager : ArtifactLifetimeManager;
        part observers : ArtifactObservationServices;
        part trace : AsyncGraphTrace;

        part lifecycle : ArtifactLifecycle;
        part dependencyPolicies : DependencyPolicyTopology;
        part executionTopology : ExecutionTopology;

        in port requests : ~RequestPort;
        in port submissions : ~SubmissionPort;
        in port builtArtifacts : ~ArtifactPort;
        out port artifacts : ArtifactPort;

        connection admissionToVersionStore connect admission.dependencyOutput to versionStore.dependencyInput;
        connection versionStoreToResolver connect versionStore.dependencyOutput to dependencyResolver.dependencyInput;
        connection resolverToScheduler connect dependencyResolver.dependencyOutput to scheduler.dependencyInput;
        connection schedulerToReadiness connect scheduler.dependencyOutput to readinessBroker.dependencyInput;
        connection readinessToVersionStore connect readinessBroker.dependencyOutput to versionStore.dependencyInput;
        connection versionStoreToLifetime connect versionStore.dependencyOutput to lifetimeManager.dependencyInput;
        connection versionStoreToObservers connect versionStore.dependencyOutput to observers.dependencyInput;
        connection admissionToTrace connect admission.dependencyOutput to trace.dependencyInput;
        connection resolverToTrace connect dependencyResolver.dependencyOutput to trace.dependencyInput;
        connection schedulerToTrace connect scheduler.dependencyOutput to trace.dependencyInput;
        connection readinessToTrace connect readinessBroker.dependencyOutput to trace.dependencyInput;
    }

    part def RequestAdmission :> DependencyNode {
        /* Exact requests preserve one immutable version. Latest intents may
           coalesce or supersede queued work for the same logical address. */
        part exactRequests : ExactRequestAdmission;
        part latestIntents : LatestIntentAdmission;
        part intentBatches : IntentBatchAdmission;
        part controlQueue : GraphControlQueue;

        connection exactRequestsToControl connect exactRequests.dependencyOutput to controlQueue.dependencyInput;
        connection latestIntentsToControl connect latestIntents.dependencyOutput to controlQueue.dependencyInput;
        connection intentBatchesToControl connect intentBatches.dependencyOutput to controlQueue.dependencyInput;
    }
    part def ExactRequestAdmission :> DependencyNode;
    part def LatestIntentAdmission :> DependencyNode;
    part def IntentBatchAdmission :> DependencyNode;
    part def GraphControlQueue :> DependencyNode;

    part def VersionStore :> DependencyNode;
    part def DependencyResolver :> DependencyNode {
        /* AllOf, AnyOf, Optional and FallbackAllowed selection;
           ExactSnapshot, Latest, ReadyGate and LifetimeHold invalidation. */
    }
    part def ProducerScheduler :> DependencyNode {
        /* FrameCritical/Streaming/Background lanes and serialized task domains. */
    }
    part def ReadinessBroker :> DependencyNode {
        /* Missing -> Blocked -> Queued -> Preparing -> CpuReady ->
           UploadSubmitted -> GpuReady -> Published, plus terminal states. */
    }
    part def ArtifactLifetimeManager :> DependencyNode;
    part def ArtifactObservationServices :> DependencyNode {
        /* Address/kind observations are diagnostic. Exact-version awaiters are
           correctness primitives registered atomically with their first sample. */
        part addressObservers : AddressObservers;
        part kindObservers : KindObservers;
        part exactAwaiters : ExactVersionAwaiters;
    }
    part def AddressObservers;
    part def KindObservers;
    part def ExactVersionAwaiters;
    part def AsyncGraphTrace :> DependencyNode;

    part def ArtifactLifecycle {
        part missingStage : MissingStage;
        part blockedStage : BlockedStage;
        part queuedStage : QueuedStage;
        part preparingStage : PreparingStage;
        part cpuReadyStage : CpuReadyStage;
        part uploadSubmittedStage : UploadSubmittedStage;
        part gpuReadyStage : GpuReadyStage;
        part publishedStage : PublishedStage;
        part terminalStages : TerminalStages;

        connection missingToBlocked connect missingStage.dependencyOutput to blockedStage.dependencyInput;
        connection blockedToQueued connect blockedStage.dependencyOutput to queuedStage.dependencyInput;
        connection queuedToPreparing connect queuedStage.dependencyOutput to preparingStage.dependencyInput;
        connection preparingToCpuReady connect preparingStage.dependencyOutput to cpuReadyStage.dependencyInput;
        connection cpuReadyToUploadSubmitted connect cpuReadyStage.dependencyOutput to uploadSubmittedStage.dependencyInput;
        connection uploadSubmittedToGpuReady connect uploadSubmittedStage.dependencyOutput to gpuReadyStage.dependencyInput;
        connection gpuReadyToPublished connect gpuReadyStage.dependencyOutput to publishedStage.dependencyInput;
        connection blockedToTerminal connect blockedStage.dependencyOutput to terminalStages.dependencyInput;
        connection queuedToTerminal connect queuedStage.dependencyOutput to terminalStages.dependencyInput;
        connection preparingToTerminal connect preparingStage.dependencyOutput to terminalStages.dependencyInput;
        connection cpuReadyToTerminal connect cpuReadyStage.dependencyOutput to terminalStages.dependencyInput;
        connection uploadSubmittedToTerminal connect uploadSubmittedStage.dependencyOutput to terminalStages.dependencyInput;
    }
    part def MissingStage :> DependencyNode;
    part def BlockedStage :> DependencyNode;
    part def QueuedStage :> DependencyNode;
    part def PreparingStage :> DependencyNode;
    part def CpuReadyStage :> DependencyNode;
    part def UploadSubmittedStage :> DependencyNode;
    part def GpuReadyStage :> DependencyNode;
    part def PublishedStage :> DependencyNode;
    part def TerminalStages :> DependencyNode {
        part supersededOutcome : SupersededOutcome;
        part cancelledOutcome : CancelledOutcome;
        part failedOutcome : FailedOutcome;
    }
    part def SupersededOutcome;
    part def CancelledOutcome;
    part def FailedOutcome;

    part def DependencyPolicyTopology {
        part selection : DependencySelectionPolicies;
        part invalidation : DependencyInvalidationPolicies;

        connection selectionToInvalidation connect selection.dependencyOutput to invalidation.dependencyInput;
    }
    part def DependencySelectionPolicies :> DependencyNode {
        part allOfPolicy : AllOfPolicy;
        part anyOfPolicy : AnyOfPolicy;
        part optionalPolicy : OptionalPolicy;
        part fallbackPolicy : FallbackAllowedPolicy;
    }
    part def AllOfPolicy;
    part def AnyOfPolicy;
    part def OptionalPolicy;
    part def FallbackAllowedPolicy;
    part def DependencyInvalidationPolicies :> DependencyNode {
        part exactSnapshotPolicy : ExactSnapshotPolicy;
        part latestPolicy : LatestPolicy;
        part readyGatePolicy : ReadyGatePolicy;
        part lifetimeHoldPolicy : LifetimeHoldPolicy;
    }
    part def ExactSnapshotPolicy;
    part def LatestPolicy;
    part def ReadyGatePolicy;
    part def LifetimeHoldPolicy;

    part def ExecutionTopology {
        part lanes : SchedulingLanes;
        part domains : SerializedTaskDomains;
        part admissionControl : BoundedAdmission;
        part suspensions : SuspensionBroker;

        connection lanesToAdmission connect lanes.dependencyOutput to admissionControl.dependencyInput;
        connection domainsToAdmission connect domains.dependencyOutput to admissionControl.dependencyInput;
        connection admissionToSuspensions connect admissionControl.dependencyOutput to suspensions.dependencyInput;
    }
    part def SchedulingLanes :> DependencyNode {
        part frameCriticalLane : FrameCriticalLane;
        part streamingLane : StreamingLane;
        part backgroundLane : BackgroundLane;
    }
    part def FrameCriticalLane;
    part def StreamingLane;
    part def BackgroundLane;
    part def SerializedTaskDomains :> DependencyNode {
        /* General, RendererState, static/asset import, texture processing,
           shader compilation, cleanup, graph control/publication, material
           acceptance, GPU-buffer build and static-group preparation. */
    }
    part def BoundedAdmission :> DependencyNode;
    part def SuspensionBroker :> DependencyNode {
        part exactDependencySuspension : ExactDependencySuspension;
        part capacitySuspension : CapacitySuspension;
        part externalOperationSuspension : ExternalOperationSuspension;
        part transientRetrySuspension : TransientRetrySuspension;
    }
    part def ExactDependencySuspension;
    part def CapacitySuspension;
    part def ExternalOperationSuspension;
    part def TransientRetrySuspension;

    part def ArtifactProducers {
        part textureBinding : TextureBindingProducer;
        part textureImages : TextureImageTableProducer;
        part gpuBuffers : VersionedGpuBufferProducer;
        part materialRows : MaterialRowProducer;
        part materialRoot : MaterialRootProducer;
        part materialUsage : MaterialUsageProducer;
        part geometry : GeometryProducer;
        part geometryResidency : GeometryResidencyProducer;
        part drawRecords : DrawRecordProducer;
        part activeLists : ActiveDrawListProducer;
        part indirect : IndirectWorkloadProducer;
        part terrain : TerrainProducer;
        part staticScene : StaticSceneProducer;
        part views : ViewFamilyProducer;
        part poses : PoseProducer;
        part lights : LightProducer;

        in port dependencies : ~ArtifactPort;
        out port artifacts : ArtifactPort;
        out port submissions : SubmissionPort;
        out port fragments : FragmentPort;

        /* Family-level dependency topology. Individual requests refine these
           into Exact, Latest, ReadyGate or LifetimeHold requirements. */
        connection textureBindingToMaterialUsage connect textureBinding.dependencyOutput to materialUsage.dependencyInput;
        connection textureBindingToTerrain connect textureBinding.dependencyOutput to terrain.dependencyInput;
        connection gpuBuffersToMaterialRoot connect gpuBuffers.dependencyOutput to materialRoot.dependencyInput;
        connection gpuBuffersToGeometry connect gpuBuffers.dependencyOutput to geometry.dependencyInput;
        connection gpuBuffersToDrawRecords connect gpuBuffers.dependencyOutput to drawRecords.dependencyInput;
        connection gpuBuffersToActiveLists connect gpuBuffers.dependencyOutput to activeLists.dependencyInput;
        connection gpuBuffersToIndirect connect gpuBuffers.dependencyOutput to indirect.dependencyInput;
        connection gpuBuffersToTerrain connect gpuBuffers.dependencyOutput to terrain.dependencyInput;
        connection gpuBuffersToViews connect gpuBuffers.dependencyOutput to views.dependencyInput;
        connection gpuBuffersToPoses connect gpuBuffers.dependencyOutput to poses.dependencyInput;
        connection gpuBuffersToLights connect gpuBuffers.dependencyOutput to lights.dependencyInput;
        connection drawRecordsToIndirect connect drawRecords.dependencyOutput to indirect.dependencyInput;
        connection activeListsToIndirect connect activeLists.dependencyOutput to indirect.dependencyInput;
        connection materialRootToIndirect connect materialRoot.dependencyOutput to indirect.dependencyInput;
        connection viewsToIndirect connect views.dependencyOutput to indirect.dependencyInput;
        connection viewsToLights connect views.dependencyOutput to lights.dependencyInput;
        connection geometryToStaticScene connect geometry.dependencyOutput to staticScene.dependencyInput;
        connection geometryResidencyToStaticScene connect geometryResidency.dependencyOutput to staticScene.dependencyInput;
        connection materialRootToStaticScene connect materialRoot.dependencyOutput to staticScene.dependencyInput;
        connection drawRecordsToStaticScene connect drawRecords.dependencyOutput to staticScene.dependencyInput;
        connection indirectToStaticScene connect indirect.dependencyOutput to staticScene.dependencyInput;
    }

    part def TextureBindingProducer :> DependencyNode;
    part def TextureImageTableProducer :> DependencyNode;
    part def VersionedGpuBufferProducer :> DependencyNode;
    part def MaterialRowProducer :> DependencyNode;
    part def MaterialRootProducer :> DependencyNode;
    part def MaterialUsageProducer :> DependencyNode;
    part def GeometryProducer :> DependencyNode;
    part def GeometryResidencyProducer :> DependencyNode;
    part def DrawRecordProducer :> DependencyNode;
    part def ActiveDrawListProducer :> DependencyNode;
    part def IndirectWorkloadProducer :> DependencyNode;
    part def TerrainProducer :> DependencyNode;
    part def StaticSceneProducer :> DependencyNode;
    part def ViewFamilyProducer :> DependencyNode;
    part def PoseProducer :> DependencyNode;
    part def LightProducer :> DependencyNode;

    part def ManifestAssembly {
        part fragmentSelection : FragmentSelection;
        part closureValidation : ClosureValidation;
        part manifestProducer : ManifestProducer;

        in port artifacts : ~ArtifactPort;
        in port fragments : ~FragmentPort;
        out port manifest : ManifestPort;

        connection selectionToValidation connect fragmentSelection.dependencyOutput to closureValidation.dependencyInput;
        connection validationToManifest connect closureValidation.dependencyOutput to manifestProducer.dependencyInput;
    }

    part def FragmentSelection :> DependencyNode;
    part def ClosureValidation :> DependencyNode;
    part def ManifestProducer :> DependencyNode;

    part def PublicationBoundary {
        part publisher : RendererStatePublisher;
        part activeState : PublishedRendererState;
        part frameSlotLeases : FrameSlotLeases;

        in port manifest : ~ManifestPort;
        out port leaseOutput : StateLeasePort;

        connection publisherToActiveState connect publisher.dependencyOutput to activeState.dependencyInput;
        connection activeStateToFrameSlots connect activeState.dependencyOutput to frameSlotLeases.dependencyInput;
    }

    part def RendererStatePublisher :> DependencyNode;
    part def PublishedRendererState :> DependencyNode;
    part def FrameSlotLeases :> DependencyNode;

    part def DependencyAndExecutionModel {
        part policies : DependencyPolicyTopology;
        part execution : ExecutionTopology;
    }

    part def PublicationPipeline {
        part manifest : ManifestAssembly;
        part publication : PublicationBoundary;

        connection manifestToPublication connect manifest.manifest to publication.manifest;
    }

    part def AsyncStateSystem {
        part sources : RequestSources;
        part graph : AsyncStateGraphKernel;
        part producers : ArtifactProducers;
        part manifest : ManifestAssembly;
        part publication : PublicationBoundary;
        part frameConsumers : AcceptedFrameConsumers;

        connection requestsToGraph connect sources.requests to graph.requests;
        connection graphToProducers connect graph.artifacts to producers.dependencies;
        connection builtArtifactsToGraph connect producers.artifacts to graph.builtArtifacts;
        connection submissionsToGraph connect producers.submissions to graph.submissions;
        connection graphToManifest connect graph.artifacts to manifest.artifacts;
        connection fragmentsToManifest connect producers.fragments to manifest.fragments;
        connection manifestToPublication connect manifest.manifest to publication.manifest;
        connection publicationToFrames connect publication.leaseOutput to frameConsumers.leaseInput;
    }

    part def AcceptedFrameConsumers {
        in port leaseInput : ~StateLeasePort;
    }

    part asyncState : AsyncStateSystem;
    part artifactLifecycleModel : ArtifactLifecycle;
    part dependencyAndExecutionModel : DependencyAndExecutionModel;
    part producerDependencyModel : ArtifactProducers;
    part publicationPipelineModel : PublicationPipeline;

    view asyncStateOverview : StandardViewDefinitions::GeneralView {
        expose asyncState;
    }

    view asyncStateInterconnections : StandardViewDefinitions::InterconnectionView {
        expose asyncState;
    }

    view artifactLifecycle : StandardViewDefinitions::InterconnectionView {
        expose artifactLifecycleModel;
    }

    view dependencyAndExecutionPolicies : StandardViewDefinitions::InterconnectionView {
        expose dependencyAndExecutionModel;
    }

    view producerDependencyGraph : StandardViewDefinitions::InterconnectionView {
        expose producerDependencyModel;
    }

    view publicationPipeline : StandardViewDefinitions::InterconnectionView {
        expose publicationPipelineModel;
    }
}
