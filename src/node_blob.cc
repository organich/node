#include "node_blob.h"
#include "ada.h"
#include "async_wrap-inl.h"
#include "base_object-inl.h"
#include "env-inl.h"
#include "memory_tracker-inl.h"
#include "node_bob-inl.h"
#include "node_errors.h"
#include "node_external_reference.h"
#include "node_file.h"
#include "util.h"
#include "v8.h"

#include <algorithm>

namespace node {

using v8::Array;
using v8::ArrayBuffer;
using v8::ArrayBufferView;
using v8::BackingStore;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Global;
using v8::HandleScope;
using v8::Int32;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::ObjectTemplate;
using v8::String;
using v8::Uint32;
using v8::Undefined;
using v8::Value;

namespace {

// Concatenate multiple ArrayBufferView/ArrayBuffers into a single ArrayBuffer.
// This method treats all ArrayBufferView types the same.
void Concat(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsArray());
  Local<Array> array = args[0].As<Array>();

  struct View {
    std::shared_ptr<BackingStore> store;
    size_t length;
    size_t offset = 0;
  };

  std::vector<View> views;
  size_t total = 0;

  for (uint32_t n = 0; n < array->Length(); n++) {
    Local<Value> val;
    if (!array->Get(env->context(), n).ToLocal(&val)) return;
    if (val->IsArrayBuffer()) {
      auto ab = val.As<ArrayBuffer>();
      views.push_back(View{ab->GetBackingStore(), ab->ByteLength(), 0});
      total += ab->ByteLength();
    } else {
      CHECK(val->IsArrayBufferView());
      auto view = val.As<ArrayBufferView>();
      views.push_back(View{view->Buffer()->GetBackingStore(),
                           view->ByteLength(),
                           view->ByteOffset()});
      total += view->ByteLength();
    }
  }

  std::shared_ptr<BackingStore> store =
      ArrayBuffer::NewBackingStore(env->isolate(), total);
  uint8_t* ptr = static_cast<uint8_t*>(store->Data());
  for (size_t n = 0; n < views.size(); n++) {
    uint8_t* from =
        static_cast<uint8_t*>(views[n].store->Data()) + views[n].offset;
    std::copy(from, from + views[n].length, ptr);
    ptr += views[n].length;
  }

  args.GetReturnValue().Set(ArrayBuffer::New(env->isolate(), std::move(store)));
}

void BlobFromFilePath(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  auto entry = DataQueue::CreateFdEntry(env, args[0]);
  if (entry == nullptr) {
    return THROW_ERR_INVALID_ARG_VALUE(env, "Unabled to open file as blob");
  }

  std::vector<std::unique_ptr<DataQueue::Entry>> entries;
  entries.push_back(std::move(entry));

  auto blob =
      Blob::Create(env, DataQueue::CreateIdempotent(std::move(entries)));

  if (blob) {
    auto array = Array::New(env->isolate(), 2);
    USE(array->Set(env->context(), 0, blob->object()));
    USE(array->Set(env->context(),
                   1,
                   Uint32::NewFromUnsigned(env->isolate(), blob->length())));

    args.GetReturnValue().Set(array);
  }
}
}  // namespace

void Blob::CreatePerIsolateProperties(IsolateData* isolate_data,
                                      Local<ObjectTemplate> target) {
  Isolate* isolate = isolate_data->isolate();

  SetMethod(isolate, target, "createBlob", New);
  SetMethod(isolate, target, "storeDataObject", StoreDataObject);
  SetMethod(isolate, target, "getDataObject", GetDataObject);
  SetMethod(isolate, target, "revokeObjectURL", RevokeObjectURL);
  SetMethod(isolate, target, "concat", Concat);
  SetMethod(isolate, target, "createBlobFromFilePath", BlobFromFilePath);
}

void Blob::CreatePerContextProperties(Local<Object> target,
                                      Local<Value> unused,
                                      Local<Context> context,
                                      void* priv) {
  Realm* realm = Realm::GetCurrent(context);
  realm->AddBindingData<BlobBindingData>(context, target);
}

Local<FunctionTemplate> Blob::GetConstructorTemplate(Environment* env) {
  Local<FunctionTemplate> tmpl = env->blob_constructor_template();
  if (tmpl.IsEmpty()) {
    Isolate* isolate = env->isolate();
    tmpl = NewFunctionTemplate(isolate, nullptr);
    tmpl->InstanceTemplate()->SetInternalFieldCount(
        BaseObject::kInternalFieldCount);
    tmpl->SetClassName(
        FIXED_ONE_BYTE_STRING(env->isolate(), "Blob"));
    SetProtoMethod(isolate, tmpl, "getReader", GetReader);
    SetProtoMethod(isolate, tmpl, "slice", ToSlice);
    env->set_blob_constructor_template(tmpl);
  }
  return tmpl;
}

bool Blob::HasInstance(Environment* env, v8::Local<v8::Value> object) {
  return GetConstructorTemplate(env)->HasInstance(object);
}

BaseObjectPtr<Blob> Blob::Create(Environment* env,
                                 std::shared_ptr<DataQueue> data_queue) {
  HandleScope scope(env->isolate());

  Local<Function> ctor;
  if (!GetConstructorTemplate(env)->GetFunction(env->context()).ToLocal(&ctor))
    return BaseObjectPtr<Blob>();

  Local<Object> obj;
  if (!ctor->NewInstance(env->context()).ToLocal(&obj))
    return BaseObjectPtr<Blob>();

  return MakeBaseObject<Blob>(env, obj, data_queue);
}

void Blob::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsArray());  // sources

  Local<Array> array = args[0].As<Array>();
  std::vector<std::unique_ptr<DataQueue::Entry>> entries(array->Length());

  for (size_t i = 0; i < array->Length(); i++) {
    Local<Value> entry;
    if (!array->Get(env->context(), i).ToLocal(&entry)) {
      return;
    }

    const auto entryFromArrayBuffer = [env](v8::Local<v8::ArrayBuffer> buf,
                                            size_t byte_length,
                                            size_t byte_offset = 0) {
      if (buf->IsDetachable()) {
        std::shared_ptr<BackingStore> store = buf->GetBackingStore();
        USE(buf->Detach(Local<Value>()));
        return DataQueue::CreateInMemoryEntryFromBackingStore(
            store, byte_offset, byte_length);
      }

      // If the ArrayBuffer is not detachable, we will copy from it instead.
      std::shared_ptr<BackingStore> store =
          ArrayBuffer::NewBackingStore(env->isolate(), byte_length);
      uint8_t* ptr = static_cast<uint8_t*>(buf->Data()) + byte_offset;
      std::copy(ptr, ptr + byte_length, static_cast<uint8_t*>(store->Data()));
      return DataQueue::CreateInMemoryEntryFromBackingStore(
          store, 0, byte_length);
    };

    // Every entry should be either an ArrayBuffer, ArrayBufferView, or Blob.
    // If the input to the Blob constructor in JavaScript was a string, then
    // it will be decoded into an ArrayBufferView there before being passed
    // in.
    //
    // Importantly, here we also assume that the ArrayBuffer/ArrayBufferView
    // is not going to be modified here so we will detach them. It is up to
    // the JavaScript side to do the right thing with regards to copying and
    // ensuring appropriate spec compliance.
    if (entry->IsArrayBuffer()) {
      Local<ArrayBuffer> buf = entry.As<ArrayBuffer>();
      entries[i] = entryFromArrayBuffer(buf, buf->ByteLength());
    } else if (entry->IsArrayBufferView()) {
      Local<ArrayBufferView> view = entry.As<ArrayBufferView>();
      entries[i] = entryFromArrayBuffer(
          view->Buffer(), view->ByteLength(), view->ByteOffset());
    } else if (Blob::HasInstance(env, entry)) {
      Blob* blob;
      ASSIGN_OR_RETURN_UNWRAP(&blob, entry);
      entries[i] = DataQueue::CreateDataQueueEntry(blob->data_queue_);
    } else {
      UNREACHABLE("Incorrect Blob initialization type");
    }
  }

  auto blob = Create(env, DataQueue::CreateIdempotent(std::move(entries)));
  if (blob)
    args.GetReturnValue().Set(blob->object());
}

void Blob::GetReader(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Blob* blob;
  ASSIGN_OR_RETURN_UNWRAP(&blob, args.Holder());

  BaseObjectPtr<Blob::Reader> reader =
      Blob::Reader::Create(env, BaseObjectPtr<Blob>(blob));
  if (reader) args.GetReturnValue().Set(reader->object());
}

void Blob::ToSlice(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Blob* blob;
  ASSIGN_OR_RETURN_UNWRAP(&blob, args.Holder());
  CHECK(args[0]->IsUint32());
  CHECK(args[1]->IsUint32());
  size_t start = args[0].As<Uint32>()->Value();
  size_t end = args[1].As<Uint32>()->Value();
  BaseObjectPtr<Blob> slice = blob->Slice(env, start, end);
  if (slice)
    args.GetReturnValue().Set(slice->object());
}

void Blob::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("data_queue_", data_queue_, "std::shared_ptr<DataQueue>");
}

BaseObjectPtr<Blob> Blob::Slice(Environment* env, size_t start, size_t end) {
  return Create(env,
                this->data_queue_->slice(start, static_cast<uint64_t>(end)));
}

Blob::Blob(Environment* env,
           v8::Local<v8::Object> obj,
           std::shared_ptr<DataQueue> data_queue)
    : BaseObject(env, obj), data_queue_(data_queue) {
  MakeWeak();
}

Blob::Reader::Reader(Environment* env,
                     v8::Local<v8::Object> obj,
                     BaseObjectPtr<Blob> strong_ptr)
    : AsyncWrap(env, obj, AsyncWrap::PROVIDER_BLOBREADER),
      inner_(strong_ptr->data_queue_->get_reader()),
      strong_ptr_(std::move(strong_ptr)) {
  MakeWeak();
}

bool Blob::Reader::HasInstance(Environment* env, v8::Local<v8::Value> value) {
  return GetConstructorTemplate(env)->HasInstance(value);
}

Local<FunctionTemplate> Blob::Reader::GetConstructorTemplate(Environment* env) {
  Local<FunctionTemplate> tmpl = env->blob_reader_constructor_template();
  if (tmpl.IsEmpty()) {
    Isolate* isolate = env->isolate();
    tmpl = NewFunctionTemplate(isolate, nullptr);
    tmpl->InstanceTemplate()->SetInternalFieldCount(
        BaseObject::kInternalFieldCount);
    tmpl->SetClassName(FIXED_ONE_BYTE_STRING(env->isolate(), "BlobReader"));
    SetProtoMethod(env->isolate(), tmpl, "pull", Pull);
    env->set_blob_reader_constructor_template(tmpl);
  }
  return tmpl;
}

BaseObjectPtr<Blob::Reader> Blob::Reader::Create(Environment* env,
                                                 BaseObjectPtr<Blob> blob) {
  Local<Object> obj;
  if (!GetConstructorTemplate(env)
           ->InstanceTemplate()
           ->NewInstance(env->context())
           .ToLocal(&obj)) {
    return BaseObjectPtr<Blob::Reader>();
  }

  return MakeBaseObject<Blob::Reader>(env, obj, std::move(blob));
}

void Blob::Reader::Pull(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Blob::Reader* reader;
  ASSIGN_OR_RETURN_UNWRAP(&reader, args.Holder());

  CHECK(args[0]->IsFunction());
  Local<Function> fn = args[0].As<Function>();
  CHECK(!fn->IsConstructor());

  if (reader->eos_) {
    Local<Value> arg = Int32::New(env->isolate(), bob::STATUS_EOS);
    reader->MakeCallback(fn, 1, &arg);
    return args.GetReturnValue().Set(bob::STATUS_EOS);
  }

  struct Impl {
    BaseObjectPtr<Blob::Reader> reader;
    Global<Function> callback;
    Environment* env;
  };
  // TODO(@jasnell): A unique_ptr is likely better here but making this a unique
  // pointer that is passed into the lambda causes the std::move(next) below to
  // complain about std::function needing to be copy-constructible.
  Impl* impl = new Impl();
  impl->reader = BaseObjectPtr<Blob::Reader>(reader);
  impl->callback.Reset(env->isolate(), fn);
  impl->env = env;

  auto next = [impl](int status,
                     const DataQueue::Vec* vecs,
                     size_t count,
                     bob::Done doneCb) mutable {
    auto dropMe = std::unique_ptr<Impl>(impl);
    Environment* env = impl->env;
    HandleScope handleScope(env->isolate());
    Local<Function> fn = impl->callback.Get(env->isolate());

    if (status == bob::STATUS_EOS) impl->reader->eos_ = true;

    if (count > 0) {
      // Copy the returns vectors into a single ArrayBuffer.
      size_t total = 0;
      for (size_t n = 0; n < count; n++) total += vecs[n].len;

      std::shared_ptr<BackingStore> store =
          v8::ArrayBuffer::NewBackingStore(env->isolate(), total);
      auto ptr = static_cast<uint8_t*>(store->Data());
      for (size_t n = 0; n < count; n++) {
        std::copy(vecs[n].base, vecs[n].base + vecs[n].len, ptr);
        ptr += vecs[n].len;
      }
      // Since we copied the data buffers, signal that we're done with them.
      std::move(doneCb)(0);
      Local<Value> argv[2] = {Uint32::New(env->isolate(), status),
                              ArrayBuffer::New(env->isolate(), store)};
      impl->reader->MakeCallback(fn, arraysize(argv), argv);
      return;
    }

    Local<Value> argv[2] = {
        Int32::New(env->isolate(), status),
        Undefined(env->isolate()),
    };
    impl->reader->MakeCallback(fn, arraysize(argv), argv);
  };

  args.GetReturnValue().Set(reader->inner_->Pull(
      std::move(next), node::bob::OPTIONS_END, nullptr, 0));
}

BaseObjectPtr<BaseObject>
Blob::BlobTransferData::Deserialize(
    Environment* env,
    Local<Context> context,
    std::unique_ptr<worker::TransferData> self) {
  if (context != env->context()) {
    THROW_ERR_MESSAGE_TARGET_CONTEXT_UNAVAILABLE(env);
    return {};
  }
  return Blob::Create(env, data_queue);
}

BaseObject::TransferMode Blob::GetTransferMode() const {
  return BaseObject::TransferMode::kCloneable;
}

std::unique_ptr<worker::TransferData> Blob::CloneForMessaging() const {
  return std::make_unique<BlobTransferData>(data_queue_);
}

void Blob::StoreDataObject(const v8::FunctionCallbackInfo<v8::Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  BlobBindingData* binding_data = Realm::GetBindingData<BlobBindingData>(args);

  CHECK(args[0]->IsString());  // ID key
  CHECK(Blob::HasInstance(env, args[1]));  // Blob
  CHECK(args[2]->IsUint32());  // Length
  CHECK(args[3]->IsString());  // Type

  Utf8Value key(env->isolate(), args[0]);
  Blob* blob;
  ASSIGN_OR_RETURN_UNWRAP(&blob, args[1]);

  size_t length = args[2].As<Uint32>()->Value();
  Utf8Value type(env->isolate(), args[3]);

  binding_data->store_data_object(
      std::string(*key, key.length()),
      BlobBindingData::StoredDataObject(
        BaseObjectPtr<Blob>(blob),
        length,
        std::string(*type, type.length())));
}

// TODO(@anonrig): Add V8 Fast API to the following function
void Blob::RevokeObjectURL(const FunctionCallbackInfo<Value>& args) {
  CHECK_GE(args.Length(), 1);
  CHECK(args[0]->IsString());
  BlobBindingData* binding_data = Realm::GetBindingData<BlobBindingData>(args);
  Environment* env = Environment::GetCurrent(args);
  Utf8Value input(env->isolate(), args[0].As<String>());
  auto out = ada::parse<ada::url_aggregator>(input.ToStringView());

  if (!out) {
    return;
  }

  auto pathname = out->get_pathname();
  auto start_index = pathname.find(':');

  if (start_index != std::string_view::npos && start_index != pathname.size()) {
    auto end_index = pathname.find(':', start_index + 1);
    if (end_index == std::string_view::npos) {
      auto id = std::string(pathname.substr(start_index + 1));
      binding_data->revoke_data_object(id);
    }
  }
}

void Blob::GetDataObject(const v8::FunctionCallbackInfo<v8::Value>& args) {
  BlobBindingData* binding_data = Realm::GetBindingData<BlobBindingData>(args);

  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsString());

  Utf8Value key(env->isolate(), args[0]);

  BlobBindingData::StoredDataObject stored =
      binding_data->get_data_object(std::string(*key, key.length()));
  if (stored.blob) {
    Local<Value> type;
    if (!String::NewFromUtf8(
            env->isolate(),
            stored.type.c_str(),
            v8::NewStringType::kNormal,
            static_cast<int>(stored.type.length())).ToLocal(&type)) {
      return;
    }

    Local<Value> values[] = {
      stored.blob->object(),
      Uint32::NewFromUnsigned(env->isolate(), stored.length),
      type
    };

    args.GetReturnValue().Set(
        Array::New(
            env->isolate(),
            values,
            arraysize(values)));
  }
}

void BlobBindingData::StoredDataObject::MemoryInfo(
    MemoryTracker* tracker) const {
  tracker->TrackField("blob", blob, "BaseObjectPtr<Blob>");
}

BlobBindingData::StoredDataObject::StoredDataObject(
    const BaseObjectPtr<Blob>& blob_,
    size_t length_,
    const std::string& type_)
    : blob(blob_),
      length(length_),
      type(type_) {}

BlobBindingData::BlobBindingData(Realm* realm, Local<Object> wrap)
    : SnapshotableObject(realm, wrap, type_int) {
  MakeWeak();
}

void BlobBindingData::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("data_objects_",
                      data_objects_,
                      "std::unordered_map<std::string, StoredDataObject>");
}

void BlobBindingData::store_data_object(
    const std::string& uuid,
    const BlobBindingData::StoredDataObject& object) {
  data_objects_[uuid] = object;
}

void BlobBindingData::revoke_data_object(const std::string& uuid) {
  if (data_objects_.find(uuid) == data_objects_.end()) {
    return;
  }
  data_objects_.erase(uuid);
  CHECK_EQ(data_objects_.find(uuid), data_objects_.end());
}

BlobBindingData::StoredDataObject BlobBindingData::get_data_object(
    const std::string& uuid) {
  auto entry = data_objects_.find(uuid);
  if (entry == data_objects_.end())
    return BlobBindingData::StoredDataObject {};
  return entry->second;
}

void BlobBindingData::Deserialize(Local<Context> context,
                                  Local<Object> holder,
                                  int index,
                                  InternalFieldInfoBase* info) {
  DCHECK_EQ(index, BaseObject::kEmbedderType);
  HandleScope scope(context->GetIsolate());
  Realm* realm = Realm::GetCurrent(context);
  BlobBindingData* binding =
      realm->AddBindingData<BlobBindingData>(context, holder);
  CHECK_NOT_NULL(binding);
}

bool BlobBindingData::PrepareForSerialization(Local<Context> context,
                                              v8::SnapshotCreator* creator) {
  // Stored blob objects are not actually persisted.
  // Return true because we need to maintain the reference to the binding from
  // JS land.
  return true;
}

InternalFieldInfoBase* BlobBindingData::Serialize(int index) {
  DCHECK_EQ(index, BaseObject::kEmbedderType);
  InternalFieldInfo* info =
      InternalFieldInfoBase::New<InternalFieldInfo>(type());
  return info;
}

void Blob::RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(Blob::New);
  registry->Register(Blob::GetReader);
  registry->Register(Blob::ToSlice);
  registry->Register(Blob::StoreDataObject);
  registry->Register(Blob::GetDataObject);
  registry->Register(Blob::RevokeObjectURL);
  registry->Register(Blob::Reader::Pull);
  registry->Register(Concat);
  registry->Register(BlobFromFilePath);
}

}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(blob,
                                    node::Blob::CreatePerContextProperties)
NODE_BINDING_PER_ISOLATE_INIT(blob, node::Blob::CreatePerIsolateProperties)
NODE_BINDING_EXTERNAL_REFERENCE(blob, node::Blob::RegisterExternalReferences)
