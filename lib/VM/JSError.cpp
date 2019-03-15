/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "hermes/VM/JSError.h"

#include "hermes/BCGen/HBC/DebugInfo.h"
#include "hermes/Support/OptValue.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/RuntimeModule-inline.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringView.h"

namespace hermes {
namespace vm {
//===----------------------------------------------------------------------===//
// class JSError

ObjectVTable JSError::vt{
    VTable(CellKind::ErrorKind, sizeof(JSError), JSError::_finalizeImpl),
    JSError::_getOwnIndexedRangeImpl,
    JSError::_haveOwnIndexedImpl,
    JSError::_getOwnIndexedPropertyFlagsImpl,
    JSError::_getOwnIndexedImpl,
    JSError::_setOwnIndexedImpl,
    JSError::_deleteOwnIndexedImpl,
    JSError::_checkAllOwnIndexedImpl,
};

void ErrorBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  ObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const JSError *>(cell);
  mb.addField("@funcNames", &self->funcNames_);
  mb.addField("@domains", &self->domains_);
}

CallResult<HermesValue> JSError::create(
    Runtime *runtime,
    Handle<JSObject> parentHandle) {
  auto propStorage =
      JSObject::createPropStorage(runtime, NEEDED_PROPERTY_SLOTS);
  if (LLVM_UNLIKELY(propStorage == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  void *mem =
      runtime->alloc</*fixedSize*/ true, HasFinalizer::Yes>(sizeof(JSError));
  return HermesValue::encodeObjectValue(new (mem) JSError(
      runtime,
      *parentHandle,
      runtime->getHiddenClassForPrototypeRaw(*parentHandle),
      **propStorage));
}

ExecutionStatus JSError::setupStack(
    Handle<JSError> selfHandle,
    Runtime *runtime) {
  // Lazily allocate the accessor.
  if (runtime->jsErrorStackAccessor.isUndefined()) {
    // This code path allocates quite a few handles, so make sure we
    // don't disturb the parent GCScope and free them.
    GCScope gcScope{runtime};

    auto getter = NativeFunction::create(
        runtime,
        Handle<JSObject>::vmcast(&runtime->functionPrototype),
        nullptr,
        JSError::errorStackGetter,
        SymbolID{},
        0,
        runtime->makeNullHandle<JSObject>());

    auto setter = NativeFunction::create(
        runtime,
        Handle<JSObject>::vmcast(&runtime->functionPrototype),
        nullptr,
        JSError::errorStackSetter,
        SymbolID{},
        1,
        runtime->makeNullHandle<JSObject>());

    auto crtRes = PropertyAccessor::create(runtime, getter, setter);
    if (crtRes == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }

    runtime->jsErrorStackAccessor = *crtRes;
  }

  auto accessor =
      Handle<PropertyAccessor>::vmcast(&runtime->jsErrorStackAccessor);

  DefinePropertyFlags dpf{};
  dpf.setEnumerable = 1;
  dpf.setConfigurable = 1;
  dpf.setGetter = 1;
  dpf.setSetter = 1;
  dpf.enumerable = 0;
  dpf.configurable = 1;

  auto res = JSObject::defineOwnProperty(
      selfHandle,
      runtime,
      Predefined::getSymbolID(Predefined::stack),
      dpf,
      accessor);
  (void)res;
  assert(
      res != ExecutionStatus::EXCEPTION && *res &&
      "defineOwnProperty() failed");
  return ExecutionStatus::RETURNED;
}

ExecutionStatus JSError::setMessage(
    Handle<JSError> selfHandle,
    Runtime *runtime,
    Handle<> message) {
  auto stringMessage = Handle<StringPrimitive>::dyn_vmcast(runtime, message);
  if (LLVM_UNLIKELY(!stringMessage)) {
    auto strRes = toString(runtime, message);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    stringMessage = toHandle(runtime, std::move(*strRes));
  }

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.setEnumerable = 1;
  dpf.enumerable = 0;
  return JSObject::defineOwnProperty(
             selfHandle,
             runtime,
             Predefined::getSymbolID(Predefined::message),
             dpf,
             stringMessage)
      .getStatus();
}

/// \return a list of function names associated with the call stack \p frames.
/// Function names are read out of the 'name' property of each Callable on the
/// stack, except that accessors are skipped. If a Callable does not have a
/// name, or if the name is an accessor, undefined is set. Names are returned in
/// reverse order (topmost frame is first).
/// In case of error returns a nullptr handle.
/// \param skipTopFrame if true, skip the top frame.
static Handle<PropStorage> getCallStackFunctionNames(
    Runtime *runtime,
    bool skipTopFrame,
    size_t sizeHint) {
  auto arrRes = PropStorage::create(runtime, sizeHint);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    runtime->clearThrownValue();
    return runtime->makeNullHandle<PropStorage>();
  }
  MutableHandle<PropStorage> names{runtime, vmcast<PropStorage>(*arrRes)};

  GCScope gcScope(runtime);
  MutableHandle<> name{runtime};
  auto marker = gcScope.createMarker();

  uint32_t frameIndex = 0;
  uint32_t namesIndex = 0;
  for (StackFramePtr cf : runtime->getStackFrames()) {
    if (frameIndex++ == 0 && skipTopFrame)
      continue;

    name = HermesValue::encodeUndefinedValue();
    if (auto callableHandle = Handle<Callable>::dyn_vmcast(
            runtime, Handle<>(&cf.getCalleeClosureOrCBRef()))) {
      NamedPropertyDescriptor desc;
      JSObject *propObj = JSObject::getNamedDescriptor(
          callableHandle,
          runtime,
          Predefined::getSymbolID(Predefined::name),
          desc);
      if (propObj && !desc.flags.accessor)
        name = JSObject::getNamedSlotValue(propObj, desc);
    } else if (cf.getCalleeClosureOrCBRef().isNativeValue()) {
      auto *cb =
          cf.getCalleeClosureOrCBRef().getNativePointer<const CodeBlock>();
      if (cb->getName().isValid())
        name = HermesValue::encodeStringValue(
            runtime->getStringPrimFromSymbolID(cb->getName()));
    }
    if (PropStorage::resize(names, runtime, namesIndex + 1) ==
        ExecutionStatus::EXCEPTION) {
      runtime->clearThrownValue();
      return runtime->makeNullHandle<PropStorage>();
    }
    names->at(namesIndex).set(name.getHermesValue(), &runtime->getHeap());
    ++namesIndex;
    gcScope.flushToMarker(marker);
  }

  return std::move(names);
}

ExecutionStatus JSError::recordStackTrace(
    Handle<JSError> selfHandle,
    Runtime *runtime,
    bool skipTopFrame,
    CodeBlock *codeBlock,
    const Inst *ip) {
  if (selfHandle->stacktrace_)
    return ExecutionStatus::RETURNED;

  auto frames = runtime->getStackFrames();

  // Check if the top frame is a JSFunction and we don't have the current
  // CodeBlock, do nothing.
  if (!skipTopFrame && !codeBlock && frames.begin() != frames.end() &&
      frames.begin()->getCalleeCodeBlock()) {
    return ExecutionStatus::RETURNED;
  }

  StackTracePtr stack{new StackTrace()};
  auto domainsRes = ArrayStorage::create(runtime, 1);
  if (LLVM_UNLIKELY(domainsRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto domains = runtime->makeMutableHandle<ArrayStorage>(
      vmcast<ArrayStorage>(*domainsRes));

  // Add the domain to the domains list, provided that it's not the same as the
  // last domain in the list. This allows us to save storage with a constant
  // time check, but we don't have to loop through and check every domain to
  // deduplicate.
  auto addDomain = [&domains,
                    runtime](CodeBlock *codeBlock) -> ExecutionStatus {
    Domain *domainPtr = codeBlock->getRuntimeModule()->getDomainUnsafe();
    if (domains->size() > 0 &&
        vmcast<Domain>(domains->at(domains->size() - 1)) == domainPtr) {
      return ExecutionStatus::RETURNED;
    }
    GCScopeMarkerRAII marker{runtime};
    Handle<Domain> domain = runtime->makeHandle(domainPtr);
    return ArrayStorage::push_back(domains, runtime, domain);
  };

  if (!skipTopFrame) {
    if (codeBlock) {
      stack->emplace_back(codeBlock, codeBlock->getOffsetOf(ip));
      if (LLVM_UNLIKELY(addDomain(codeBlock) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    } else {
      stack->emplace_back(nullptr, 0);
    }
  }

  // Fill in the call stack.
  // Each stack frame tracks information about the caller.
  for (StackFramePtr cf : runtime->getStackFrames()) {
    auto *savedCodeBlock = cf.getSavedCodeBlock();
    stack->emplace_back(
        savedCodeBlock,
        savedCodeBlock ? savedCodeBlock->getOffsetOf(cf.getSavedIP()) : 0);
    if (savedCodeBlock) {
      if (LLVM_UNLIKELY(
              addDomain(savedCodeBlock) == ExecutionStatus::EXCEPTION)) {
        return ExecutionStatus::EXCEPTION;
      }
    }
  }

  // Remove the last entry.
  stack->pop_back();

  auto funcNames =
      getCallStackFunctionNames(runtime, skipTopFrame, stack->size());

  // Either the function names is empty, or they have the same count.
  assert(
      (!funcNames || funcNames->size() == stack->size()) &&
      "Function names and stack trace must have same size.");

  selfHandle->stacktrace_ = std::move(stack);
  selfHandle->funcNames_.set(*funcNames, &runtime->getHeap());
  return ExecutionStatus::RETURNED;
}

/// Given a codeblock and opcode offset, \returns the debug information.
static OptValue<hbc::DebugSourceLocation> getDebugInfo(
    CodeBlock *codeBlock,
    uint32_t bytecodeOffset) {
  auto offset = codeBlock->getDebugSourceLocationsOffset();
  if (!offset.hasValue()) {
    return llvm::None;
  }

  return codeBlock->getRuntimeModule()
      ->getBytecode()
      ->getDebugInfo()
      ->getLocationForAddress(offset.getValue(), bytecodeOffset);
}

bool JSError::appendFunctionNameAtIndex(
    Runtime *runtime,
    Handle<JSError> selfHandle,
    size_t index,
    llvm::SmallVectorImpl<char16_t> &str) {
  IdentifierTable &idt = runtime->getIdentifierTable();
  MutableHandle<StringPrimitive> name{
      runtime, runtime->getPredefinedString(Predefined::emptyString)};

  // If funcNames_ is set and contains a string primitive, use that.
  if (selfHandle->funcNames_) {
    assert(index < selfHandle->funcNames_->size() && "Index out of bounds");
    name = dyn_vmcast<StringPrimitive>(selfHandle->funcNames_->at(index));
  }

  if (!name || name->getStringLength() == 0) {
    // We did not have an explicit function name, or it was not a nonempty
    // string. If we have a code block, try its debug info.
    if (const CodeBlock *codeBlock =
            selfHandle->stacktrace_->at(index).codeBlock) {
      name = idt.getStringPrim(runtime, codeBlock->getName());
    }
  }

  if (!name || name->getStringLength() == 0)
    return false;

  name->copyUTF16String(str);
  return true;
}

void JSError::constructStackTraceString(
    Runtime *runtime,
    Handle<JSError> selfHandle,
    SmallU16String<32> &stack) {
  GCScope gcScope(runtime);
  // First of all, the stacktrace string starts with error.toString.
  auto res = toString(runtime, selfHandle);
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    // If toString throws an exception, we just use <error>.
    stack.append(u"<error>");
    // There is not much we can do if exception thrown when trying to
    // get the stacktrace. We just name it <error>, and it should be
    // sufficient to tell what happened here.
    runtime->clearThrownValue();
  } else {
    res->get()->copyUTF16String(stack);
  }

  // Virtual offsets are computed by walking the list of bytecode functions. If
  // we have an extremely deep stack, this could get expensive. Assume that very
  // deep stacks are most likely due to runaway recursion and so use a local
  // cache of virtual offsets.
  llvm::DenseMap<const CodeBlock *, uint32_t> virtualOffsetCache;

  // Append each function location in the call stack to stack trace.
  auto marker = gcScope.createMarker();
  for (size_t index = 0, max = selfHandle->stacktrace_->size(); index < max;
       index++) {
    char buf[NUMBER_TO_STRING_BUF_SIZE];

    // If the trace contains more than 100 entries, limit the string to the
    // first 50 and the last 50 entries and include a line about the truncation.
    static constexpr unsigned PRINT_HEAD = 50;
    static constexpr unsigned PRINT_TAIL = 50;
    if (LLVM_UNLIKELY(max > PRINT_HEAD + PRINT_TAIL)) {
      if (index == PRINT_HEAD) {
        stack.append("\n    ... skipping ");
        numberToString(max - PRINT_HEAD - PRINT_TAIL, buf, sizeof(buf));
        stack.append(buf);
        stack.append(" frames");
        continue;
      }

      // Skip the middle frames.
      if (index > PRINT_HEAD && index < max - PRINT_TAIL) {
        index = max - PRINT_TAIL;
      }
    }

    const StackTraceInfo &sti = selfHandle->stacktrace_->at(index);
    gcScope.flushToMarker(marker);
    // For each stacktrace entry, we add a line with the following format:
    // at <functionName> (<fileName>:<lineNo>:<columnNo>)

    stack.append(u"\n    at ");

    if (!appendFunctionNameAtIndex(runtime, selfHandle, index, stack))
      stack.append(u"anonymous");

    // If we have a null codeBlock, it's a native function, which do not have
    // lines and columns.
    if (!sti.codeBlock) {
      stack.append(u" (native)");
      continue;
    }

    // We are not a native function.
    int32_t lineNo;
    int32_t columnNo;
    OptValue<SymbolID> fileName;
    bool isAddress = false;
    OptValue<hbc::DebugSourceLocation> location =
        getDebugInfo(sti.codeBlock, sti.bytecodeOffset);
    if (location) {
      // Use the line and column from the debug info.
      lineNo = location->line;
      columnNo = location->column;
    } else {
      // Use a "line" and "column" synthesized from the bytecode.
      // In our synthesized stack trace, a line corresponds to a bytecode
      // module. This matches the interpretation in DebugInfo.cpp. Currently we
      // can only have one bytecode module without debug information, namely the
      // one loaded from disk, which is always at index 1.
      // TODO: find a way to track the bytecode modules explicitly.
      // TODO: we do not yet have a way of getting the file name separate from
      // the debug info. For now we end up leaving it as "unknown".
      auto pair = virtualOffsetCache.insert({sti.codeBlock, 0});
      uint32_t &virtualOffset = pair.first->second;
      if (pair.second) {
        // Code block was not in the cache, update the cache.
        virtualOffset = sti.codeBlock->getVirtualOffset();
      }
      lineNo = 1;
      columnNo = sti.bytecodeOffset + virtualOffset;
      isAddress = true;
    }

    stack.append(u" (");
    if (isAddress)
      stack.append(u"address at ");

    // Append the filename. If we have a source location, use the filename from
    // that location; otherwise use the RuntimeModule's sourceURL; otherwise
    // report unknown.
    RuntimeModule *runtimeModule = sti.codeBlock->getRuntimeModule();
    if (location) {
      stack.append(
          runtimeModule->getBytecode()->getDebugInfo()->getFilenameByID(
              location->filenameId));
    } else {
      auto sourceURL = runtimeModule->getSourceURL();
      stack.append(sourceURL.empty() ? "unknown" : sourceURL);
    }
    stack.push_back(u':');

    numberToString(lineNo, buf, NUMBER_TO_STRING_BUF_SIZE);
    stack.append(buf);

    stack.push_back(u':');

    numberToString(columnNo, buf, NUMBER_TO_STRING_BUF_SIZE);
    stack.append(buf);

    stack.push_back(u')');
  }
}

CallResult<HermesValue>
JSError::errorStackGetter(void *, Runtime *runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSError>(runtime);
  if (!selfHandle) {
    return runtime->raiseTypeError(
        "Error.stack accessor 'this' must be an instance of 'Error'");
  }

  if (!selfHandle->stacktrace_) {
    // Stacktrace has not been set, we simply return empty string.
    // This is different from other VMs where stacktrace is created when
    // the error object is created. We only set it when the error
    // is raised.
    return HermesValue::encodeStringValue(
        runtime->getPredefinedString(Predefined::emptyString));
  }
  SmallU16String<32> stack;
  constructStackTraceString(runtime, selfHandle, stack);

// After the stacktrace string is constructed, only the debugger may want the
// internal stacktrace_; if there is no debugger it can be freed. We no longer
// need the accessor. Redefines the stack property to a regular property.
#ifndef HERMES_ENABLE_DEBUGGER
  selfHandle->stacktrace_.reset();
#endif

  MutableHandle<> stacktraceStr{runtime};
  auto strRes = StringPrimitive::create(runtime, stack);
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    // StringPrimitive creation can throw if the stacktrace string is too long.
    // In that case, we replace it with a predefined string.
    stacktraceStr = HermesValue::encodeStringValue(
        runtime->getPredefinedString(Predefined::stacktraceTooLong));
    runtime->clearThrownValue();
  } else {
    stacktraceStr = *strRes;
  }

  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.setEnumerable = 1;
  dpf.enumerable = 0;
  if (JSObject::defineOwnProperty(
          selfHandle,
          runtime,
          Predefined::getSymbolID(Predefined::stack),
          dpf,
          stacktraceStr) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return stacktraceStr.getHermesValue();
}

CallResult<HermesValue>
JSError::errorStackSetter(void *, Runtime *runtime, NativeArgs args) {
  if (auto *errorObject = dyn_vmcast<JSError>(args.getThisArg())) {
    auto &stacktrace = errorObject->stacktrace_;
    if (stacktrace) {
      // Release stacktrace_ if it's already set.
      stacktrace.reset();
    }
  }
  auto res = toObject(runtime, args.getThisHandle());
  if (LLVM_UNLIKELY(res == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto selfHandle = runtime->makeHandle<JSObject>(res.getValue());

  // Redefines the stack property to a regular property.
  DefinePropertyFlags dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.setEnumerable = 1;
  dpf.enumerable = 0;
  if (JSObject::defineOwnProperty(
          selfHandle,
          runtime,
          Predefined::getSymbolID(Predefined::stack),
          dpf,
          args.getArgHandle(runtime, 0)) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  return HermesValue::encodeUndefinedValue();
}

void JSError::_finalizeImpl(GCCell *cell, GC *) {
  JSError *self = vmcast<JSError>(cell);
  self->~JSError();
}

} // namespace vm
} // namespace hermes
