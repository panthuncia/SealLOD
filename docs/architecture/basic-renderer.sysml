package BasicRendererArchitecture {
    /* Passive values crossing the major architecture boundaries. */
    item def SceneChange;
    item def AssetData;
    item def ArtifactIntent;
    item def PublishedStateLease;
    item def FrameInputs;
    item def RenderGraphWork;
    item def GpuCommands;

    /* Ports name the contract; subsystem-specific models refine their payloads. */
    port def SceneChangePort { out item change : SceneChange; }
    port def AssetPort { out item asset : AssetData; }
    port def ArtifactIntentPort { out item intent : ArtifactIntent; }
    port def PublicationPort { out item publishedLease : PublishedStateLease; }
    port def FramePort { out item frameInputs : FrameInputs; }
    port def GraphWorkPort { out item work : RenderGraphWork; }
    port def CommandPort { out item commands : GpuCommands; }

    part def SceneAndAssetSources {
        part sceneModel : SceneModel;
        part importPipeline : ImportPipeline;
        part featureInputs : FeatureInputs;

        out port sceneChanges : SceneChangePort;
        out port assets : AssetPort;
    }

    part def SceneModel {
        /* BasicScene plus renderer-owned SceneSourceStateStore. */
    }

    part def ImportPipeline {
        /* Assimp, glTF, USD, NIF/BRNifly, CLOD caches and preprocessing. */
    }

    part def FeatureInputs {
        /* Environment, terrain, materials, textures, meshes, objects, views,
           lights, skeletons/poses, procedural wind and contributor input. */
    }

    part def RendererOrchestration {
        part ingestion : SceneIngestion;
        part persistentServices : PersistentServices;
        part stateProduction : StateProduction;
        part frameCoordinator : FrameCoordinator;
        part featureGraph : RenderingFeatures;

        in port sceneChanges : ~SceneChangePort;
        in port assets : ~AssetPort;
        out port acceptedFrame : FramePort;
        out port graphWork : GraphWorkPort;

        /* Current implementation hotspot: Renderer owns/composes most parts. */
        connection ingestionToServices connect ingestion to persistentServices;
        connection intentsToStateProduction connect ingestion.intents to stateProduction.intents;
        connection servicesToStateProduction connect persistentServices to stateProduction;
        connection publicationToFrameCoordinator connect stateProduction.publication to frameCoordinator.publication;
        connection frameCoordinatorToFeatures connect frameCoordinator to featureGraph;
    }

    part def SceneIngestion {
        /* Ordered source revisions and narrow materialization/residency/request services. */
        out port intents : ArtifactIntentPort;
    }

    part def PersistentServices {
        part resources : ResourceServices;
        part io : IoServices;
        part programs : ProgramServices;
        part vendor : VendorServices;
    }

    part def ResourceServices {
        /* Stable identity/range allocation, descriptor heaps, backing pools and caches. */
    }

    part def IoServices {
        /* Upload, readback, texture production/streaming and DirectStorage. */
    }

    part def ProgramServices {
        /* Shader variants, PSOs, command signatures and work-graph programs. */
    }

    part def VendorServices {
        /* FidelityFX, Streamline and other generation-owned SDK contexts. */
    }

    part def StateProduction {
        part asyncGraph : AsyncStateGraph;
        part publisher : RendererStatePublisher;

        in port intents : ~ArtifactIntentPort;
        out port publication : PublicationPort;

        connection graphToPublisher connect asyncGraph to publisher;
    }

    part def AsyncStateGraph {
        /* Persistent immutable artifact versions, readiness, scheduling,
           invalidation, suspension, cancellation and retained ownership. */
    }

    part def RendererStatePublisher {
        /* Monotonic manifest fragments, resource catalog and frame-slot leases. */
    }

    part def FrameCoordinator {
        /* Accepts one publication lease and immutable inputs for each frame slot. */
        in port publication : ~PublicationPort;
    }

    part def RenderingFeatures {
        part corePipeline : CorePipeline;
        part clusterLod : ClusterLodPipeline;
        part virtualShadows : VirtualShadowPipeline;
        part transparency : TransparencyPipeline;
        part postAndVendor : PostProcessingPipeline;
        part extensions : ExtensionHost;
    }

    part def CorePipeline;
    part def ClusterLodPipeline;
    part def VirtualShadowPipeline;
    part def TransparencyPipeline;
    part def PostProcessingPipeline;
    part def ExtensionHost;

    part def OpenRenderGraphRuntime {
        part declaration : GraphDeclaration;
        part compiler : GraphCompiler;
        part frameRuntime : FrameRuntime;
        part contributorHost : ContributorHost;

        in port acceptedFrame : ~FramePort;
        in port work : ~GraphWorkPort;
        out port commands : CommandPort;

        connection declarationToCompiler connect declaration to compiler;
        connection compilerToRuntime connect compiler to frameRuntime;
        connection contributorsToDeclaration connect contributorHost to declaration;
    }

    part def GraphDeclaration;
    part def GraphCompiler {
        /* Dependency ordering, queue assignment, barriers and alias placement. */
    }
    part def FrameRuntime {
        /* Bounded slots, preparation, recording, submission and retirement. */
    }
    part def ContributorHost;

    part def PlatformRuntime {
        part rhi : BasicRHI;
        part moduleServices : ORGModuleServices;
        part telemetry : BasicTelemetry;

        in port commands : ~CommandPort;
    }

    part def BasicRHI {
        /* D3D12/Vulkan devices, resources, descriptors, queues, fences and swap chain. */
    }
    part def ORGModuleServices {
        /* Shared render-graph-facing device/module services. */
    }
    part def BasicTelemetry;

    part def BasicRendererSystem {
        part sources : SceneAndAssetSources;
        part renderer : RendererOrchestration;
        part graph : OpenRenderGraphRuntime;
        part platform : PlatformRuntime;

        connection sceneChangesToRenderer connect sources.sceneChanges to renderer.sceneChanges;
        connection assetsToRenderer connect sources.assets to renderer.assets;
        connection acceptedFrameToGraph connect renderer.acceptedFrame to graph.acceptedFrame;
        connection graphWorkToGraph connect renderer.graphWork to graph.work;
        connection commandsToPlatform connect graph.commands to platform.commands;

    }

    part basicRenderer : BasicRendererSystem;

    view basicRendererHierarchy : StandardViewDefinitions::GeneralView {
        expose basicRenderer;
    }

    view basicRendererInterconnections : StandardViewDefinitions::InterconnectionView {
        expose basicRenderer;
    }
}
