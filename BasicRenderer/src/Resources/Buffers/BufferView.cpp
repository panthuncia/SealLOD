#include "Resources/Buffers/BufferView.h"

#include "Resources/Buffers/DynamicBufferBase.h"

std::shared_ptr<org::ViewedDynamicBufferBase> org::BufferView::GetBuffer() const {
	return m_buffer.lock();
}
