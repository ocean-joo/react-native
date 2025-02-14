/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <memory>
#include <string>

#include <fbjni/fbjni.h>
#include <jsi/jsi.h>

#include <ReactCommon/TurboCxxModule.h>
#include <ReactCommon/TurboModuleBinding.h>
#include <ReactCommon/TurboModulePerfLogger.h>

#include "TurboModuleManager.h"

namespace facebook {
namespace react {

TurboModuleManager::TurboModuleManager(
    jni::alias_ref<TurboModuleManager::javaobject> jThis,
    RuntimeExecutor runtimeExecutor,
    std::shared_ptr<CallInvoker> jsCallInvoker,
    std::shared_ptr<CallInvoker> nativeCallInvoker,
    jni::alias_ref<TurboModuleManagerDelegate::javaobject> delegate,
    bool useGlobalCallbackCleanupScopeUsingRetainJSCallback,
    bool useTurboModuleManagerCallbackCleanupScope)
    : javaPart_(jni::make_global(jThis)),
      runtimeExecutor_(runtimeExecutor),
      jsCallInvoker_(jsCallInvoker),
      nativeCallInvoker_(nativeCallInvoker),
      delegate_(jni::make_global(delegate)),
      turboModuleCache_(std::make_shared<TurboModuleCache>()) {
  if (useGlobalCallbackCleanupScopeUsingRetainJSCallback) {
    longLivedObjectCollection_ = nullptr;
    retainJSCallback_ = [](jsi::Function &&callback,
                           jsi::Runtime &runtime,
                           std::shared_ptr<CallInvoker> jsInvoker) {
      return CallbackWrapper::createWeak(
          std::move(callback), runtime, jsInvoker);
    };
  } else if (useTurboModuleManagerCallbackCleanupScope) {
    longLivedObjectCollection_ = std::make_shared<LongLivedObjectCollection>();
    retainJSCallback_ = [longLivedObjectCollection =
                             longLivedObjectCollection_](
                            jsi::Function &&callback,
                            jsi::Runtime &runtime,
                            std::shared_ptr<CallInvoker> jsInvoker) {
      return CallbackWrapper::createWeak(
          longLivedObjectCollection, std::move(callback), runtime, jsInvoker);
    };
  }
}

jni::local_ref<TurboModuleManager::jhybriddata> TurboModuleManager::initHybrid(
    jni::alias_ref<jhybridobject> jThis,
    jni::alias_ref<JRuntimeExecutor::javaobject> runtimeExecutor,
    jni::alias_ref<CallInvokerHolder::javaobject> jsCallInvokerHolder,
    jni::alias_ref<CallInvokerHolder::javaobject> nativeCallInvokerHolder,
    jni::alias_ref<TurboModuleManagerDelegate::javaobject> delegate,
    bool useGlobalCallbackCleanupScopeUsingRetainJSCallback,
    bool useTurboModuleManagerCallbackCleanupScope) {
  auto jsCallInvoker = jsCallInvokerHolder->cthis()->getCallInvoker();
  auto nativeCallInvoker = nativeCallInvokerHolder->cthis()->getCallInvoker();

  return makeCxxInstance(
      jThis,
      runtimeExecutor->cthis()->get(),
      jsCallInvoker,
      nativeCallInvoker,
      delegate,
      useGlobalCallbackCleanupScopeUsingRetainJSCallback,
      useTurboModuleManagerCallbackCleanupScope);
}

void TurboModuleManager::registerNatives() {
  registerHybrid({
      makeNativeMethod("initHybrid", TurboModuleManager::initHybrid),
      makeNativeMethod(
          "installJSIBindings", TurboModuleManager::installJSIBindings),
  });
}

void TurboModuleManager::installJSIBindings() {
  if (!jsCallInvoker_) {
    return; // Runtime doesn't exist when attached to Chrome debugger.
  }

  runtimeExecutor_([this](jsi::Runtime &runtime) {
    auto turboModuleProvider =
        [turboModuleCache_ = std::weak_ptr<TurboModuleCache>(turboModuleCache_),
         jsCallInvoker_ = std::weak_ptr<CallInvoker>(jsCallInvoker_),
         nativeCallInvoker_ = std::weak_ptr<CallInvoker>(nativeCallInvoker_),
         delegate_ = jni::make_weak(delegate_),
         javaPart_ = jni::make_weak(javaPart_),
         retainJSCallback = retainJSCallback_](
            const std::string &name) -> std::shared_ptr<TurboModule> {
      auto turboModuleCache = turboModuleCache_.lock();
      auto jsCallInvoker = jsCallInvoker_.lock();
      auto nativeCallInvoker = nativeCallInvoker_.lock();
      auto delegate = delegate_.lockLocal();
      auto javaPart = javaPart_.lockLocal();

      if (!turboModuleCache || !jsCallInvoker || !nativeCallInvoker ||
          !delegate || !javaPart) {
        return nullptr;
      }

      const char *moduleName = name.c_str();

      TurboModulePerfLogger::moduleJSRequireBeginningStart(moduleName);

      auto turboModuleLookup = turboModuleCache->find(name);
      if (turboModuleLookup != turboModuleCache->end()) {
        TurboModulePerfLogger::moduleJSRequireBeginningCacheHit(moduleName);
        TurboModulePerfLogger::moduleJSRequireBeginningEnd(moduleName);
        return turboModuleLookup->second;
      }

      TurboModulePerfLogger::moduleJSRequireBeginningEnd(moduleName);

      auto cxxModule = delegate->cthis()->getTurboModule(name, jsCallInvoker);
      if (cxxModule) {
        turboModuleCache->insert({name, cxxModule});
        return cxxModule;
      }

      static auto getLegacyCxxModule =
          javaPart->getClass()
              ->getMethod<jni::alias_ref<CxxModuleWrapper::javaobject>(
                  const std::string &)>("getLegacyCxxModule");
      auto legacyCxxModule = getLegacyCxxModule(javaPart.get(), name);

      if (legacyCxxModule) {
        TurboModulePerfLogger::moduleJSRequireEndingStart(moduleName);

        auto turboModule = std::make_shared<react::TurboCxxModule>(
            legacyCxxModule->cthis()->getModule(), jsCallInvoker);
        turboModuleCache->insert({name, turboModule});

        TurboModulePerfLogger::moduleJSRequireEndingEnd(moduleName);
        return turboModule;
      }

      static auto getJavaModule =
          javaPart->getClass()
              ->getMethod<jni::alias_ref<JTurboModule>(const std::string &)>(
                  "getJavaModule");
      auto moduleInstance = getJavaModule(javaPart.get(), name);

      if (moduleInstance) {
        TurboModulePerfLogger::moduleJSRequireEndingStart(moduleName);
        JavaTurboModule::InitParams params = {
            .moduleName = name,
            .instance = moduleInstance,
            .jsInvoker = jsCallInvoker,
            .nativeInvoker = nativeCallInvoker,
            .retainJSCallback = retainJSCallback};

        auto turboModule = delegate->cthis()->getTurboModule(name, params);
        turboModuleCache->insert({name, turboModule});
        TurboModulePerfLogger::moduleJSRequireEndingEnd(moduleName);
        return turboModule;
      }

      return nullptr;
    };

    TurboModuleBinding::install(
        runtime, std::move(turboModuleProvider), longLivedObjectCollection_);
  });
}

} // namespace react
} // namespace facebook
