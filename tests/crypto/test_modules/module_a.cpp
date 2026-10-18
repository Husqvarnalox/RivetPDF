// SPDX-License-Identifier: MPL-2.0
// Tiny loadable module used by TestModuleTrust: identifies itself as "1".
extern "C" __attribute__((visibility("default"))) int rivetTestModuleId() { return 1; }
