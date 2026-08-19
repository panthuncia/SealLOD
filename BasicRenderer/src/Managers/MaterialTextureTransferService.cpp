#include "Managers/MaterialTextureTransferService.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include <DirectXTex.h>
#include <rhi_conversions_dx12.h>
#include <rhi_interop.h>

#include <BasicTelemetry/Tracy.h>
#include <spdlog/spdlog.h>

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Resources/PixelBuffer.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/ResourceStateTracker.h"
#include "rhi_helpers.h"

namespace {
	std::vector<rhi::helpers::SubresourceData> BuildSubresources(
		const TextureDescription& description,
		const TextureFactory::TextureInitialData& initialData,
		uint32_t mipLevels,
		uint32_t arraySlices)
	{
		const uint32_t count = mipLevels * arraySlices;
		std::vector<rhi::helpers::SubresourceData> result(count);
		for (uint32_t i = 0; i < count && i < initialData.subresources.size(); ++i) {
			const auto& bytes = initialData.subresources[i];
			if (!bytes || i >= description.imageDimensions.size()) continue;
			const auto& dimensions = description.imageDimensions[i];
			if (bytes->size() < dimensions.slicePitch) {
				throw std::runtime_error("material texture subresource is smaller than its declared slice pitch");
			}
			result[i] = {
				.pData = bytes->data(),
				.rowPitch = static_cast<uint32_t>(dimensions.rowPitch),
				.slicePitch = static_cast<uint32_t>(dimensions.slicePitch),
			};
		}
		return result;
	}
}

void MaterialTextureTransferService::SaveReadbackToDds(InFlightBatch::ReadbackCompletion completion)
{
	void* mapped = nullptr;
	completion.buffer->GetAPIResource().Map(&mapped);
	if (!mapped) return;
	DirectX::ScratchImage image;
	const auto dxgiFormat = rhi::ToDxgi(completion.format);
	if (FAILED(image.Initialize2D(dxgiFormat, completion.width, completion.height, 1, completion.mipLevels))) {
		completion.buffer->GetAPIResource().Unmap(0, 0);
		return;
	}
	for (uint32_t mip = 0; mip < completion.mipLevels; ++mip) {
		const auto& footprint = completion.footprints[mip];
		const auto* destination = image.GetImage(mip, 0, 0);
		const auto* source = static_cast<const uint8_t*>(mapped) + footprint.offset;
		for (size_t row = 0; row < destination->height; ++row) {
			std::memcpy(
				destination->pixels + row * destination->rowPitch,
				source + row * footprint.rowPitch,
				(std::min)(destination->rowPitch, static_cast<size_t>(footprint.rowPitch)));
		}
	}
	completion.buffer->GetAPIResource().Unmap(0, 0);
	const HRESULT result = DirectX::SaveToDDSFile(
		image.GetImages(), image.GetImageCount(), image.GetMetadata(),
		DirectX::DDS_FLAGS_NONE, completion.outputFile.c_str());
	if (FAILED(result)) {
		spdlog::error("Material texture external readback failed to save '{}'.", std::filesystem::path(completion.outputFile).string());
	}
	if (completion.callback) completion.callback();
}

void MaterialTextureTransferService::RequestReadback(
	const std::shared_ptr<PixelBuffer>& image,
	std::wstring outputFile,
	std::function<void()> callback)
{
	if (!image) return;
	std::scoped_lock lock(m_mutex);
	m_pendingReadbacks.push_back({image, std::move(outputFile), std::move(callback)});
}

void MaterialTextureTransferService::Initialize()
{
	BT_ZONE_SCOPE("MaterialTextureTransferService::Initialize");
	std::scoped_lock lock(m_mutex);
	if (m_initialized) return;
	auto& manager = DeviceManager::GetInstance();
	m_device = manager.GetDevice();
	m_graphicsQueue = manager.GetGraphicsQueue();
	if (!m_device.IsValid() || !m_graphicsQueue.IsValid() ||
		rhi::Failed(m_device.CreateTimeline(m_timeline, 0, "MaterialTextureTransfers"))) {
		throw std::runtime_error("failed to initialize material texture transfer service");
	}
	m_initialized = true;
}

void MaterialTextureTransferService::Shutdown()
{
	BT_ZONE_SCOPE("MaterialTextureTransferService::Shutdown");
	uint64_t waitValue = 0;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_initialized) return;
		waitValue = m_nextFenceValue;
	}
	if (waitValue != 0) (void)m_timeline->HostWait(waitValue);
	std::scoped_lock lock(m_mutex);
	ReapCompletedLocked();
	m_pending.clear();
	m_pendingReadbacks.clear();
	m_inFlight.clear();
	m_records.clear();
	m_timeline.Reset();
	m_device = {};
	m_graphicsQueue = {};
	m_initialized = false;
}

void MaterialTextureTransferService::EnqueueUpload(
	const std::shared_ptr<PixelBuffer>& image,
	TextureDescription description,
	TextureFactory::TextureInitialData initialData)
{
	BT_ZONE_SCOPE("MaterialTextureTransferService::EnqueueUpload");
	if (!image || initialData.Empty()) return;
	image->SetGraphOwnership(Resource::GraphOwnership::ExternalImmutableShaderResource);
	std::scoped_lock lock(m_mutex);
	const uint64_t id = image->GetGlobalResourceID();
	if (m_records.contains(id)) return;
	m_records.emplace(id, Record{});
	m_pending.push_back({image, std::move(description), std::move(initialData), true});
	BT_PLOT("MaterialTextureTransfer.Pending", static_cast<int64_t>(m_pending.size()));
}

void MaterialTextureTransferService::EnsureShaderReady(const std::shared_ptr<PixelBuffer>& image)
{
	BT_ZONE_SCOPE("MaterialTextureTransferService::EnsureShaderReady");
	if (!image) return;
	image->SetGraphOwnership(Resource::GraphOwnership::ExternalImmutableShaderResource);
	std::scoped_lock lock(m_mutex);
	const uint64_t id = image->GetGlobalResourceID();
	if (m_records.contains(id)) return;
	m_records.emplace(id, Record{});
	m_pending.push_back({image, image->GetDescription(), {}, false});
}

rhi::TextureBarrier MaterialTextureTransferService::MakeWholeTextureBarrier(
	const PixelBuffer& image,
	rhi::ResourceAccessType beforeAccess,
	rhi::ResourceAccessType afterAccess,
	rhi::ResourceLayout beforeLayout,
	rhi::ResourceLayout afterLayout,
	rhi::ResourceSyncState beforeSync,
	rhi::ResourceSyncState afterSync)
{
	rhi::TextureBarrier barrier{};
	barrier.texture = const_cast<PixelBuffer&>(image).GetAPIResource().GetHandle();
	barrier.range = {0, image.GetMipLevels(), 0, image.GetArraySize()};
	barrier.beforeAccess = beforeAccess;
	barrier.afterAccess = afterAccess;
	barrier.beforeLayout = beforeLayout;
	barrier.afterLayout = afterLayout;
	barrier.beforeSync = beforeSync;
	barrier.afterSync = afterSync;
	return barrier;
}

void MaterialTextureTransferService::ReapCompletedLocked()
{
	if (!m_timeline) return;
	const uint64_t completed = m_timeline->GetCompletedValue();
	for (size_t i = 0; i < m_inFlight.size();) {
		auto& batch = m_inFlight[i];
		if (completed < batch.fenceValue) {
			++i;
			continue;
		}
		for (const auto& image : batch.images) {
			if (!image) continue;
			const uint64_t id = image->GetGlobalResourceID();
			auto record = m_records.find(id);
			if (record == m_records.end() || record->second.fenceValue != batch.fenceValue) continue;
			RangeSpec whole{};
			image->GetStateTracker()->Reset(
				whole,
				ResourceState{
					rhi::ResourceAccessType::ShaderResource,
					rhi::ResourceLayout::ShaderResource,
					rhi::ResourceSyncState::AllShading});
			record->second.state = State::Ready;
		}
		for (auto& readback : batch.readbacks) {
			TaskSchedulerManager::GetInstance().RunBackgroundTask(
				"MaterialTextureTransferService::SaveReadback",
				[completion = std::move(readback)]() mutable { SaveReadbackToDds(std::move(completion)); });
		}
		m_inFlight[i] = std::move(m_inFlight.back());
		m_inFlight.pop_back();
	}
}

void MaterialTextureTransferService::Pump()
{
	BT_ZONE_SCOPE("MaterialTextureTransferService::Pump");
	std::scoped_lock lock(m_mutex);
	if (!m_initialized) return;
	ReapCompletedLocked();
	if (m_pending.empty() && m_pendingReadbacks.empty()) return;

	std::vector<Request> requests;
	requests.swap(m_pending);
	std::vector<ReadbackRequest> readbacks;
	readbacks.swap(m_pendingReadbacks);
	InFlightBatch batch{};
	if (rhi::Failed(m_device.CreateCommandAllocator(rhi::QueueKind::Graphics, batch.allocator)) ||
		rhi::Failed(m_device.CreateCommandList(rhi::QueueKind::Graphics, batch.allocator.Get(), batch.commandList))) {
		for (const auto& request : requests) {
			if (request.image) m_records[request.image->GetGlobalResourceID()].state = State::Failed;
		}
		spdlog::error("MaterialTextureTransferService: failed to create graphics command list");
		return;
	}

	for (auto& request : requests) {
		if (!request.image || !request.image->HasValidBackingResource()) {
			if (request.image) m_records[request.image->GetGlobalResourceID()].state = State::Failed;
			continue;
		}
		try {
			if (request.upload) {
				if (request.description.imageDimensions.empty() || request.initialData.Empty()) {
					throw std::runtime_error("material texture upload has no subresource data");
				}
				rhi::VulkanDeviceInfo vulkanDeviceInfo{};
				const bool vulkanImageStartsUndefined = rhi::QueryNativeDevice(
					m_device,
					rhi::RHI_IID_VK_DEVICE,
					&vulkanDeviceInfo,
					sizeof(vulkanDeviceInfo));
				auto toCopy = MakeWholeTextureBarrier(
					*request.image,
					vulkanImageStartsUndefined ? rhi::ResourceAccessType::None : rhi::ResourceAccessType::Common,
					rhi::ResourceAccessType::CopyDest,
					vulkanImageStartsUndefined ? rhi::ResourceLayout::Undefined : rhi::ResourceLayout::Common,
					rhi::ResourceLayout::CopyDest,
					vulkanImageStartsUndefined ? rhi::ResourceSyncState::None : rhi::ResourceSyncState::All,
					rhi::ResourceSyncState::Copy);
				rhi::BarrierBatch barriers{};
				barriers.textures = {&toCopy, 1};
				batch.commandList->Barriers(barriers);

				const uint32_t mipLevels = request.image->GetMipLevels();
				const uint32_t arraySlices = request.image->GetArraySize();
				auto subresources = BuildSubresources(request.description, request.initialData, mipLevels, arraySlices);
				auto destination = request.image->GetAPIResource();
				auto staging = rhi::helpers::UpdateTextureSubresources(
					m_device,
					batch.commandList.Get(),
					destination,
					request.description.format,
					request.description.imageDimensions[0].width,
					request.description.imageDimensions[0].height,
					1,
					mipLevels,
					arraySlices,
					{subresources.data(), static_cast<uint32_t>(subresources.size())});
				if (!staging) {
					throw std::runtime_error("material texture staging allocation failed");
				}
				batch.stagingResources.push_back(std::move(staging));
			}

			auto toShader = MakeWholeTextureBarrier(
				*request.image,
				request.upload ? rhi::ResourceAccessType::CopyDest : rhi::ResourceAccessType::Common,
				rhi::ResourceAccessType::ShaderResource,
				request.upload ? rhi::ResourceLayout::CopyDest : rhi::ResourceLayout::Common,
				rhi::ResourceLayout::ShaderResource,
				request.upload ? rhi::ResourceSyncState::Copy : rhi::ResourceSyncState::All,
				rhi::ResourceSyncState::AllShading);
			rhi::BarrierBatch barriers{};
			barriers.textures = {&toShader, 1};
			batch.commandList->Barriers(barriers);
			batch.images.push_back(request.image);
		}
		catch (const std::exception& ex) {
			m_records[request.image->GetGlobalResourceID()].state = State::Failed;
			spdlog::error("Material texture transfer recording failed for '{}' id={}: {}",
				request.image->GetName(), request.image->GetGlobalResourceID(), ex.what());
		}
	}

	for (auto& request : readbacks) {
		if (!request.image || !request.image->HasValidBackingResource()) continue;
		const auto record = m_records.find(request.image->GetGlobalResourceID());
		if (record == m_records.end() || record->second.state != State::Ready) {
			m_pendingReadbacks.push_back(std::move(request));
			continue;
		}
		const uint32_t mipLevels = request.image->GetMipLevels();
		std::vector<rhi::CopyableFootprint> footprints(mipLevels);
		rhi::FootprintRangeDesc range{};
		range.texture = request.image->GetAPIResource().GetHandle();
		range.mipCount = mipLevels;
		range.arraySize = 1;
		const auto footprintInfo = m_device.GetCopyableFootprints(
			range, footprints.data(), static_cast<uint32_t>(footprints.size()));
		if (footprintInfo.count != mipLevels || footprintInfo.totalBytes == 0) continue;
		auto buffer = Buffer::CreateShared(rhi::HeapType::Readback, footprintInfo.totalBytes);
		buffer->SetName("External material texture readback");
		auto toCopy = MakeWholeTextureBarrier(
			*request.image,
			rhi::ResourceAccessType::ShaderResource, rhi::ResourceAccessType::CopySource,
			rhi::ResourceLayout::ShaderResource, rhi::ResourceLayout::CopySource,
			rhi::ResourceSyncState::AllShading, rhi::ResourceSyncState::Copy);
		rhi::BarrierBatch before{};
		before.textures = {&toCopy, 1};
		batch.commandList->Barriers(before);
		for (uint32_t mip = 0; mip < mipLevels; ++mip) {
			rhi::BufferTextureCopyFootprint copy{};
			copy.texture = request.image->GetAPIResource().GetHandle();
			copy.buffer = buffer->GetAPIResource().GetHandle();
			copy.mip = mip;
			copy.footprint = footprints[mip];
			batch.commandList->CopyTextureToBuffer(copy);
		}
		auto toShader = MakeWholeTextureBarrier(
			*request.image,
			rhi::ResourceAccessType::CopySource, rhi::ResourceAccessType::ShaderResource,
			rhi::ResourceLayout::CopySource, rhi::ResourceLayout::ShaderResource,
			rhi::ResourceSyncState::Copy, rhi::ResourceSyncState::AllShading);
		rhi::BarrierBatch after{};
		after.textures = {&toShader, 1};
		batch.commandList->Barriers(after);
		batch.readbacks.push_back({
			.buffer = std::move(buffer),
			.footprints = std::move(footprints),
			.width = request.image->GetWidth(),
			.height = request.image->GetHeight(),
			.mipLevels = mipLevels,
			.format = request.image->GetFormat(),
			.outputFile = std::move(request.outputFile),
			.callback = std::move(request.callback)});
	}

	if (batch.images.empty() && batch.readbacks.empty()) return;
	batch.commandList->End();
	auto commandList = batch.commandList.Get();
	if (rhi::Failed(m_graphicsQueue.Submit({&commandList, 1}))) {
		for (const auto& image : batch.images) m_records[image->GetGlobalResourceID()].state = State::Failed;
		spdlog::error("MaterialTextureTransferService: graphics submission failed");
		return;
	}
	batch.fenceValue = ++m_nextFenceValue;
	if (rhi::Failed(m_graphicsQueue.Signal({m_timeline->GetHandle(), batch.fenceValue}))) {
		// Submission already transferred ownership to the queue.  Keep every
		// allocation alive and force completion before marking the ticket failed.
		(void)m_device.WaitIdle();
		for (const auto& image : batch.images) {
			m_records[image->GetGlobalResourceID()].state = State::Failed;
		}
		spdlog::error("MaterialTextureTransferService: completion signal failed; graphics queue was drained safely");
		return;
	}
	for (const auto& image : batch.images) {
		auto& record = m_records[image->GetGlobalResourceID()];
		record.state = State::InFlight;
		record.fenceValue = batch.fenceValue;
	}
	BT_PLOT("MaterialTextureTransfer.Submitted", static_cast<int64_t>(batch.images.size()));
	m_inFlight.push_back(std::move(batch));
}

bool MaterialTextureTransferService::IsShaderReady(const std::shared_ptr<PixelBuffer>& image) const
{
	if (!image) return false;
	std::scoped_lock lock(m_mutex);
	const auto it = m_records.find(image->GetGlobalResourceID());
	return it != m_records.end() && it->second.state == State::Ready;
}

bool MaterialTextureTransferService::HasFailed(const std::shared_ptr<PixelBuffer>& image) const
{
	if (!image) return true;
	std::scoped_lock lock(m_mutex);
	const auto it = m_records.find(image->GetGlobalResourceID());
	return it != m_records.end() && it->second.state == State::Failed;
}
