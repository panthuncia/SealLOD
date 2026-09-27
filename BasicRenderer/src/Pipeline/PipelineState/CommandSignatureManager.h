#pragma once

#include <unordered_map>
#include <string>
#include <memory>

#include <rhi.h>

class CommandSignatureManager {
public:
	static CommandSignatureManager& GetInstance();
	void Initialize();
	void Cleanup();
	const rhi::CommandSignature& GetDispatchMeshCommandSignature() {
		return m_dispatchMeshCommandSignature->Get();
	}
	const rhi::CommandSignature& GetDispatchCommandSignature() {
		return m_dispatchCommandSignature->Get();
	}
	const rhi::CommandSignature& GetRawDispatchCommandSignature() {
		return m_rawDispatchCommandSignature->Get();
	}
	const rhi::CommandSignature& GetMaterialEvaluationCommandSignature() {
		return m_materialEvaluationCommandSignature->Get();
	}
	const rhi::CommandSignature& GetTerrainRegionMaterialEvaluationCommandSignature() {
		return m_terrainRegionMaterialEvaluationCommandSignature->Get();
	}

    std::shared_ptr<const rhi::CommandSignaturePtr> CaptureRawDispatchCommandSignature() const {
        return m_rawDispatchCommandSignature;
    }

    std::shared_ptr<const rhi::CommandSignaturePtr> CaptureDispatchMeshCommandSignature() const {
        return m_dispatchMeshCommandSignature;
    }

    std::shared_ptr<const rhi::CommandSignaturePtr> CaptureDispatchCommandSignature() const {
        return m_dispatchCommandSignature;
    }

    std::shared_ptr<const rhi::CommandSignaturePtr> CaptureMaterialEvaluationCommandSignature() const {
        return m_materialEvaluationCommandSignature;
    }

    std::shared_ptr<const rhi::CommandSignaturePtr> CaptureTerrainRegionMaterialEvaluationCommandSignature() const {
        return m_terrainRegionMaterialEvaluationCommandSignature;
    }

private:
	std::shared_ptr<rhi::CommandSignaturePtr> m_dispatchMeshCommandSignature;
	std::shared_ptr<rhi::CommandSignaturePtr> m_dispatchCommandSignature;
	std::shared_ptr<rhi::CommandSignaturePtr> m_rawDispatchCommandSignature;
	std::shared_ptr<rhi::CommandSignaturePtr> m_materialEvaluationCommandSignature;
	std::shared_ptr<rhi::CommandSignaturePtr> m_terrainRegionMaterialEvaluationCommandSignature;
};

inline CommandSignatureManager& CommandSignatureManager::GetInstance() {
	static CommandSignatureManager instance;
	return instance;
}
