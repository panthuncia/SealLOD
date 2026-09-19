#pragma once

#include <memory>
#include <optional>
#include <mutex>
#include <vector>
#include <functional>
#include <string>

#include "Scene/Environment.h"
#include "Render/Runtime/FrameWorkQueue.h"
#include "Render/EnvironmentWorkService.h"
#include "ShaderBuffers.h"
#include "Resources/Buffers/LazyDynamicStructuredBuffer.h"
#include "Resources/ResourceGroup.h"
#include "Resources/PixelBuffer.h"
#include "Interfaces/IResourceProvider.h"

namespace org { class BufferView; }
namespace org { class DynamicBuffer; }
namespace org { class PixelBuffer; }
namespace org::runtime { class IUploadService; }

class EnvironmentManager : public org::IResourceProvider {
public:
	using RequestReadbackFn = std::function<void(std::shared_ptr<org::PixelBuffer>, std::wstring, std::function<void()>, bool)>;

	static std::unique_ptr<EnvironmentManager> CreateUnique(std::shared_ptr<org::runtime::IUploadService> uploadService) {
		return std::unique_ptr<EnvironmentManager>(new EnvironmentManager(std::move(uploadService)));
	}
	void SetUploadService(std::shared_ptr<org::runtime::IUploadService> uploadService) { m_uploadService = std::move(uploadService); }

	void SetRequestReadbackFn(RequestReadbackFn fn) {
		m_requestReadback = std::move(fn);
	}
	void SetWorkServices(br::render::EnvironmentWorkServices services) {
		m_workServices = std::move(services);
	}

	std::unique_ptr<Environment> CreateEnvironment(std::wstring name = L"");
	void RemoveEnvironment(Environment* environment);
	

	void UpdateEnvironmentView(const Environment& environment) {
		m_environmentInfoBuffer->UpdateView(environment.GetEnvironmentBufferView(), &environment.m_environmentInfo);
	}

	void SetFromHDRI(Environment* e, std::string hdriPath);

	std::shared_ptr<org::Resource> ProvideResource(org::ResourceIdentifier const& key) override;
	std::vector<org::ResourceIdentifier> GetSupportedKeys() override;
	std::vector<org::ResourceIdentifier> GetSupportedResolverKeys() override;
	std::shared_ptr<org::IResourceResolver> ProvideResolver(org::ResourceIdentifier const& key) override;

private:
	explicit EnvironmentManager(std::shared_ptr<org::runtime::IUploadService> uploadService);
	std::shared_ptr<org::runtime::IUploadService> m_uploadService;
	std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::Resource>, org::ResourceIdentifier::Hasher> m_resources;
	std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::IResourceResolver>, org::ResourceIdentifier::Hasher> m_resolvers;

	std::shared_ptr<org::LazyDynamicStructuredBuffer<EnvironmentInfo>> m_environmentInfoBuffer;
	std::mutex m_environmentInfoBufferMutex; // Mutex for thread safety

	unsigned int m_skyboxResolution = 2048;
	unsigned int m_reflectionCubemapResolution = 512;

	// Queue ownership belongs to the renderer-scoped service. The environment
	// artifact producer only submits owned work requests into that service.
	br::render::EnvironmentWorkServices m_workServices;
	std::shared_ptr<std::mutex> m_environmentUpdateMutex = std::make_shared<std::mutex>(); // Mutex for thread safety

	std::shared_ptr<org::ResourceGroup> m_workingEnvironmentCubemapGroup; // Temporary group for prefiltered cubemap generation
	std::shared_ptr<org::ResourceGroup> m_workingHDRIGroup; // Temporary group for prefiltered cubemap generation

	std::shared_ptr<org::ResourceGroup> m_environmentPrefilteredCubemapGroup;
	RequestReadbackFn m_requestReadback;

	friend class Environment;
};
