// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "eei.h"

namespace SSVM {
namespace Executor {

class EEIStorageStore : public EEI {
public:
  EEIStorageStore(VM::EVMEnvironment &Env);
  EEIStorageStore() = delete;
  virtual ~EEIStorageStore() = default;

  virtual ErrCode run(std::vector<Value> &Args, std::vector<Value> &Res,
                      StoreManager &Store, Instance::ModuleInstance *ModInst);
};

} // namespace Executor
} // namespace SSVM
