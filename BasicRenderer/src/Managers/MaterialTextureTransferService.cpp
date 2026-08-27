#include "Managers/MaterialTextureTransferService.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>

#include <DirectXTex.h>
#include <rhi_conversions_dx12.h>
#include <rhi_interop.h>

#include <BasicTelemetry/Tracy.h>
#include <spdlog/spdlog.h>

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Render/AsyncStateGraph.h"
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
		if (!destination || destination->rowPitch == 0) {
			completion.buffer->GetAPIResource().Unmap(0, 0);
			spdlog::error("Material texture external readback has an invalid destination image at mip {}.", mip);
			return;
		}
		const auto blockInfo = rhi::GetBlockInfo(completion.format);
		const size_t rows = blockInfo.isCompressed
			? (std::max)(size_t{1}, (destination->height + blockInfo.blockHeight - 1u) / blockInfo.blockHeight)
			: destination->height;
		const uint64_t sourceEnd = footprint.offset + static_cast<uint64_t>(footprint.rowPitch) * rows;
		if (footprint.rowPitch < destination->rowPitch || sourceEnd > completion.bufferSize) {
			completion.buffer->GetAPIResource().Unmap(0, 0);
			spdlog::error(
				"Material texture external readback footprint is invalid at mip {}: offset={} rowPitch={} rows={} "
				"destinationRowPitch={} bufferSize={}.",
				mip, footprint.offset, footprint.rowPitch, rows, destination->rowPitch, completion.bufferSize);
			return;
		}
		const auto* source = static_cast<const uint8_t*>(mapped) + footprint.offset;
		for (size_t row = 0; row < rows; ++row) {
			std::memcpy(
				destination->pixels + row * destination->rowPitch,
				source + row * footprint.rowPitch,
				destination->rowPitch);
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
	m_timeline = std::make_shared<rhi::TimelinePtr>();
	if (!m_device.IsValid() || !m_graphicsQueue.IsValid() ||
		rhi::Failed(m_device.CreateTimeline(*m_timeline, 0, "MaterialTextureTransfers"))) {
		m_timeline.reset();
		throw std::runtime_error("failed to initialize material texture transfer service");
	}
	m_taskScope = TaskSchedulerManager::GetInstance().CreateScope("MaterialTextureTransferService");
	m_shuttingDown.store(false, std::memory_order_release);
	m_initialized = true;
}

void MaterialTextureTransferService::Shutdown()
{
	BT_ZONE_SCOPE("MaterialTextureTransferService::Shutdown");
	m_shuttingDown.store(true, std::memory_order_release);
	if (m_taskScope.Valid()) m_taskScope.CancelAndWait();
	m_pumpScheduled.store(false, std::memory_order_release);
	uint64_t waitValue = 0;
	{
		std::scoped_lock lock(m_mutex);
		if (!m_initialized) return;
		waitValue = m_nextFenceValue;
	}
	if (waitValue != 0 && m_timeline && *m_timeline) (void)(*m_timeline)->HostWait(waitValue);
	std::scoped_lock lock(m_mutex);
	ReapCompletedLocked();
	m_pending.clear();
	m_pendingReadbacks.clear();
	m_inFlight.clear();
	m_records.clear();
	if (m_timeline) m_timeline->Reset();
	m_timeline.reset();
	m_device = {};
	m_graphicsQueue = {};
	m_initialized = false;
}

std::shared_ptr<const br::render::TextureTransferArtifact>
MaterialTextureTransferService::EnsureTransferRecordLocked(
	const std::shared_ptr<PixelBuffer>& image)
{
	if (!image) return {};
	const auto id = image->GetGlobalResourceID();
	if (const auto found = m_records.find(id); found != m_records.end()) {
		return found->second.artifact;
	}
	auto transfer = std::make_shared<TransferState>();
	auto submissions = std::make_shared<br::render::GpuSubmissionSet>();
	auto timeline = m_timeline;
	br::render::GpuQueueSubmission queueSubmission{ timeline, 0 };
	queueSubmission.currentValue = [transfer] {
		return transfer->fenceValue.load(std::memory_order_acquire);
	};
	submissions->submissions.push_back(std::move(queueSubmission));
	submissions->isSubmitted = [transfer] {
		return transfer->state.load(std::memory_order_acquire) != State::Pending;
	};
	submissions->isComplete = [transfer, timeline] {
		const auto state = transfer->state.load(std::memory_order_acquire);
		if (state == State::Failed || state == State::Ready) return true;
		const auto value = transfer->fenceValue.load(std::memory_order_acquire);
		return state == State::InFlight && value != 0 && timeline && *timeline &&
			(*timeline)->GetCompletedValue() >= value;
	};
	submissions->isFailed = [transfer] {
		return transfer->state.load(std::memory_order_acquire) == State::Failed;
	};
	submissions->failure = [transfer] {
		std::lock_guard lock(transfer->callbackMutex);
		return transfer->error.empty() ? std::string{ "material texture transfer failed" }
			: transfer->error;
	};
	submissions->subscribe = [transfer](std::function<void()> callback) {
		if (!callback) return;
		bool invoke = false;
		{
			std::lock_guard lock(transfer->callbackMutex);
			if (transfer->state.load(std::memory_order_acquire) == State::Pending) {
				transfer->callbacks.push_back(std::move(callback));
			} else {
				invoke = true;
			}
		}
		if (invoke) callback();
	};
	auto artifact = std::make_shared<br::render::TextureTransferArtifact>();
	artifact->image = image;
	artifact->generation = id;
	artifact->gpuSubmissions = std::move(submissions);
	m_records.emplace(id, Record{ State::Pending, 0, transfer, artifact });
	return artifact;
}

void MaterialTextureTransferService::PublishTransferState(
	const std::shared_ptr<TransferState>& transfer, State state,
	std::uint64_t fenceValue, std::string error)
{
	if (!transfer) return;
	std::vector<std::function<void()>> callbacks;
	{
		std::lock_guard lock(transfer->callbackMutex);
		if (fenceValue != 0) transfer->fenceValue.store(fenceValue, std::memory_order_release);
		transfer->error = std::move(error);
		transfer->state.store(state, std::memory_order_release);
		callbacks.swap(transfer->callbacks);
	}
	for (auto& callback : callbacks) if (callback) callback();
}

std::shared_ptr<const br::render::TextureTransferArtifact>
MaterialTextureTransferService::EnqueueUpload(
	const std::shared_ptr<PixelBuffer>& image,
	TextureDescription description,
	TextureFactory::TextureInitialData initialData)
{
	BT_ZONE_SCOPE("MaterialTextureTransferService::EnqueueUpload");
	if (!image || initialData.Empty()) return {};
	image->SetGraphOwnership(Resource::GraphOwnership::ExternalImmutableShaderResource);
	std::shared_ptr<const br::render::TextureTransferArtifact> artifact;
	{
		std::scoped_lock lock(m_mutex);
		const bool existed = m_records.contains(image->GetGlobalResourceID());
		artifact = EnsureTransferRecordLocked(image);
		if (!existed) {
			m_pending.push_back({image, std::move(description), std::move(initialData), true});
			BT_PLOT("MaterialTextureTransfer.Pending", static_cast<int64_t>(m_pending.size()));
		}
	}
	Pump();
	return artifact;
}

std::shared_ptr<const br::render::TextureTransferArtifact>
MaterialTextureTransferService::EnsureShaderReady(const std::shared_ptr<PixelBuffer>& image)
{
	BT_ZONE_SCOPE("MaterialTextureTransferService::EnsureShaderReady");
	if (!image) return {};
	image->SetGraphOwnership(Resource::GraphOwnership::ExternalImmutableShaderResource);
	std::shared_ptr<const br::render::TextureTransferArtifact> artifact;
	{
		std::scoped_lock lock(m_mutex);
		const bool existed = m_records.contains(image->GetGlobalResourceID());
		artifact = EnsureTransferRecordLocked(image);
		if (!existed) m_pending.push_back({image, image->GetDescription(), {}, false});
	}
	Pump();
	return artifact;
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
	if (!m_timeline || !*m_timeline) return;
	const uint64_t completed = (*m_timeline)->GetCompletedValue();
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
			PublishTransferState(record->second.transferState, State::Ready,
				record->second.fenceValue);
		}
		for (auto& readback : batch.readbacks) {
			TaskSchedulerManager::GetInstance().Submit(
				TaskLane::Background,
				TaskDomain::Cleanup,
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
	if (m_shuttingDown.load(std::memory_order_acquire)) return;
	bool expected = false;
	if (!m_pumpScheduled.compare_exchange_strong(
		expected, true, std::memory_order_acq_rel)) return;
	if (!m_taskScope.Valid() || !TaskSchedulerManager::GetInstance().Submit(
		m_taskScope, TaskLane::Streaming, TaskDomain::TextureProcessing,
		"MaterialTextureTransferService::PumpWorker",
		[this](const br::TaskContext& context) {
			if (!context.StopRequested()) PumpWorker();
			m_pumpScheduled.store(false, std::memory_order_release);
		})) {
		m_pumpScheduled.store(false, std::memory_order_release);
	}
}

void MaterialTextureTransferService::PumpWorker()
{
	BT_ZONE_SCOPE("MaterialTextureTransferService::PumpWorker");
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
			if (request.image) {
				auto& record = m_records[request.image->GetGlobalResourceID()];
				record.state = State::Failed;
				PublishTransferState(record.transferState, State::Failed, 0,
					"failed to create transfer command list");
			}
		}
		spdlog::error("MaterialTextureTransferService: failed to create graphics command list");
		return;
	}

	for (auto& request : requests) {
		if (!request.image || !request.image->HasValidBackingResource()) {
			if (request.image) {
				auto& record = m_records[request.image->GetGlobalResourceID()];
				record.state = State::Failed;
				PublishTransferState(record.transferState, State::Failed, 0,
					"invalid texture transfer resource");
			}
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
			auto& record = m_records[request.image->GetGlobalResourceID()];
			record.state = State::Failed;
			PublishTransferState(record.transferState, State::Failed, 0, ex.what());
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
			.bufferSize = footprintInfo.totalBytes,
			.outputFile = std::move(request.outputFile),
			.callback = std::move(request.callback)});
	}

	if (batch.images.empty() && batch.readbacks.empty()) return;
	batch.commandList->End();
	auto commandList = batch.commandList.Get();
	if (rhi::Failed(m_graphicsQueue.Submit({&commandList, 1}))) {
		for (const auto& image : batch.images) {
			auto& record = m_records[image->GetGlobalResourceID()];
			record.state = State::Failed;
			PublishTransferState(record.transferState, State::Failed, 0,
				"graphics submission failed");
		}
		spdlog::error("MaterialTextureTransferService: graphics submission failed");
		return;
	}
	batch.fenceValue = ++m_nextFenceValue;
	if (!m_timeline || !*m_timeline ||
		rhi::Failed(m_graphicsQueue.Signal({(*m_timeline)->GetHandle(), batch.fenceValue}))) {
		// Submission already transferred ownership to the queue.  Keep every
		// allocation alive and force completion before marking the ticket failed.
		(void)m_device.WaitIdle();
		for (const auto& image : batch.images) {
			auto& record = m_records[image->GetGlobalResourceID()];
			record.state = State::Failed;
			PublishTransferState(record.transferState, State::Failed, 0,
				"completion signal failed");
		}
		spdlog::error("MaterialTextureTransferService: completion signal failed; graphics queue was drained safely");
		return;
	}
	for (const auto& image : batch.images) {
		auto& record = m_records[image->GetGlobalResourceID()];
		record.state = State::InFlight;
		record.fenceValue = batch.fenceValue;
		PublishTransferState(record.transferState, State::InFlight, batch.fenceValue);
	}
	BT_PLOT("MaterialTextureTransfer.Submitted", static_cast<int64_t>(batch.images.size()));
	m_inFlight.push_back(std::move(batch));
}

bool MaterialTextureTransferService::IsShaderReady(const std::shared_ptr<PixelBuffer>& image) const
{
	if (!image) return false;
	std::unique_lock lock(m_mutex, std::try_to_lock);
	if (!lock.owns_lock()) return false;
	const auto it = m_records.find(image->GetGlobalResourceID());
	return it != m_records.end() && it->second.state == State::Ready;
}

std::shared_ptr<const br::render::GpuSubmissionSet>
MaterialTextureTransferService::ShaderReadySubmission(
	const std::shared_ptr<PixelBuffer>& image) const
{
	if (!image) return {};
	std::unique_lock lock(m_mutex, std::try_to_lock);
	if (!lock.owns_lock()) return {};
	const auto found = m_records.find(image->GetGlobalResourceID());
	if (found == m_records.end() ||
		(found->second.state != State::InFlight && found->second.state != State::Ready) ||
		found->second.fenceValue == 0 || !m_timeline || !*m_timeline) return {};
	auto timeline = m_timeline;
	const auto value = found->second.fenceValue;
	auto result = std::make_shared<br::render::GpuSubmissionSet>();
	result->submissions.push_back({ timeline, value });
	result->isComplete = [timeline, value] {
		return timeline && *timeline && (*timeline)->GetCompletedValue() >= value;
	};
	result->describe = [value] {
		return std::format("material-texture-transfer value={}", value);
	};
	return result;
}

bool MaterialTextureTransferService::HasFailed(const std::shared_ptr<PixelBuffer>& image) const
{
	if (!image) return true;
	std::unique_lock lock(m_mutex, std::try_to_lock);
	if (!lock.owns_lock()) return false;
	const auto it = m_records.find(image->GetGlobalResourceID());
	return it != m_records.end() && it->second.state == State::Failed;
}
