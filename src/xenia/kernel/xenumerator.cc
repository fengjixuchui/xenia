/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xenumerator.h"

namespace xe {
namespace kernel {

XEnumerator::XEnumerator(KernelState* kernel_state, size_t items_per_enumerate,
                         size_t item_size)
    : XObject(kernel_state, kObjectType),
      items_per_enumerate_(items_per_enumerate),
      item_size_(item_size) {}

XEnumerator::~XEnumerator() = default;

X_STATUS XEnumerator::Initialize(uint32_t user_index, uint32_t app_id,
                                 uint32_t message, uint32_t message2,
                                 uint32_t flags, uint32_t extra_size,
                                 void** extra_buffer) {
  auto native_object = CreateNative(sizeof(X_KENUMERATOR) + extra_size);
  if (!native_object) {
    return X_STATUS_NO_MEMORY;
  }
  auto guest_object = reinterpret_cast<X_KENUMERATOR*>(native_object);
  guest_object->app_id = app_id;
  guest_object->message = message;
  guest_object->message2 = message2;
  guest_object->user_index = user_index;
  guest_object->items_per_enumerate =
      static_cast<uint32_t>(items_per_enumerate_);
  guest_object->flags = flags;
  if (extra_buffer) {
    *extra_buffer =
        !extra_buffer ? nullptr : &native_object[sizeof(X_KENUMERATOR)];
  }
  return X_STATUS_SUCCESS;
}

X_STATUS XEnumerator::Initialize(uint32_t user_index, uint32_t app_id,
                                 uint32_t message, uint32_t message2,
                                 uint32_t flags) {
  return Initialize(user_index, app_id, message, message2, flags, 0, nullptr);
}

uint8_t* XStaticEnumerator::AppendItem() {
  buffer_.resize(++item_count_ * item_size());
  auto ptr =
      const_cast<uint8_t*>(buffer_.data() + (item_count_ - 1) * item_size());
  return ptr;
}

uint32_t XStaticEnumerator::WriteItems(uint32_t buffer_ptr,
                                       uint8_t* buffer_data,
                                       uint32_t buffer_size,
                                       uint32_t* written_count) {
  size_t count = std::min(item_count_ - current_item_, items_per_enumerate());
  if (!count) {
    return X_ERROR_NO_MORE_FILES;
  }

  size_t size = count * item_size();
  if (size > buffer_size) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }

  size_t offset = current_item_ * item_size();
  std::memcpy(buffer_data, buffer_.data() + offset, size);

  current_item_ += count;

  if (written_count) {
    *written_count = static_cast<uint32_t>(count);
  }

  return X_ERROR_SUCCESS;
}

}  // namespace kernel
}  // namespace xe
