#pragma once

#if USE_DX11
#include <cstdint>
struct ID3D11Device;

struct IdentityTestResult
{
    unsigned cases = 0;
    uint64_t pixels = 0;
    uint64_t mismatches = 0;
    long error = 0;
};

IdentityTestResult run_identity_selftest(ID3D11Device* sourceDevice);
long run_srgb_selftest(ID3D11Device* sourceDevice, uint8_t values[2][4]);
#endif
