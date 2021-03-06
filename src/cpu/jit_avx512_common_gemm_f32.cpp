/*******************************************************************************
* Copyright 2017 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <math.h>

#include "mkldnn_thread.hpp"
#include "utils.hpp"

#include "jit_avx512_common_gemm_f32.hpp"

#define CACHE_LINE_SIZE 16

namespace mkldnn {
namespace impl {
namespace cpu {

using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::utils;

using namespace Xbyak;
#define STACKSIZE get_size_of_abi_save_regs()
#ifdef _WIN32
#define STACK_K_CAPACITY 32
#else
#define STACK_K_CAPACITY 2048
#endif
#define SIZE 4
#define OFFSET 128
#define BASE_SHIFT 2
#define SECOND_FETCH unroll_n
#define UNROLL_M 48
#define UNROLL_N 8

struct jit_avx512_common_gemm_f32::xbyak_gemm : public jit_generator {
    xbyak_gemm(char transa, char transb, float beta, bool hasBias = false,
            void *code_ptr = nullptr,
            size_t code_size = 80 * Xbyak::DEFAULT_MAX_CODE_SIZE)
        : jit_generator(code_ptr, code_size)
    {
        enum { ver_avx512_core, ver_avx512_mic } ver =
            mayiuse(avx512_core) ? ver_avx512_core : ver_avx512_mic;

        bool isTransA = (transa == 'T' || transa == 't');
        bool isTransB = (transb == 'T' || transb == 't');
        bool isBeta0 = (beta == 0.0);
        bool isBetaN = (!isBeta0 && beta != 1.0);

        // various definitions for convenience
        auto ARG_M = abi_param1;
        auto ARG_N = abi_param2;
        auto K = abi_param3;
        auto ARG_ALPHA = abi_param4;
#ifdef _WIN32
        auto ARG_A = ptr[rsp + OFFSET_SHADOWSPACE + STACKSIZE];
        auto ARG_LDA = qword[rsp + OFFSET_SHADOWSPACE +
            sizeof(float *) + STACKSIZE];
        const auto stackOffset = OFFSET_SHADOWSPACE +
            sizeof(float *) + STACKSIZE;
        auto A = rsi;
        auto LDA = rdi;
#else
        auto ARG_A = r8;
        auto ARG_LDA = r9;
        const auto stackOffset = STACKSIZE;
        auto A = ARG_A;
        auto LDA = ARG_LDA;
#endif
        auto ARG_B = ptr[rsp + 8 + stackOffset];
        auto ARG_LDB = ptr[rsp + 16 + stackOffset];
        auto ARG_BETA = ptr[rsp + 24 + stackOffset];
        auto ARG_C = ptr[rsp + 32 + stackOffset];
        auto ARG_LDC = ptr[rsp + 40 + stackOffset];
        auto ARG_BIAS = ptr[rsp + 48 + stackOffset];
        auto ARG_WS = ptr[rsp + 56 + stackOffset];

        auto B = r11;
        auto LDB = rbx;
        auto LDC = r13;
        auto LL = rax;
        auto AO1 = abi_param2;
        auto BO1 = abi_param4;
        auto BO2 = rbp;
        auto CO1 = r14;
        auto CO2 = r15;
        auto LDB3 = r10;
        auto LDA4 = abi_param1;
        auto AA = r12;
        auto BIAS1 = abi_param1;

        auto M = qword[rsp + 0];
        auto N = qword[rsp + 8];
        auto FLAG = qword[rsp + 16];
        auto I = qword[rsp + 24];
        auto C = qword[rsp + 32];
        auto BIAS = qword[rsp + 40];
        auto ALPHA = qword[rsp + 48];
        auto BETA = qword[rsp + 64];
        auto ORIG_A = qword[rsp + 80];
        auto ORIG_SP = qword[rsp + 120];

        auto ZSTRIDE = zmm4;
        auto VALPHA = zmm6;
        auto VBETA = zmm7;
        auto VBIAS1 = zmm1;
        auto VBIAS2 = zmm2;
        auto VBIAS3 = zmm3;

        auto PREFETCHSIZEA = ver == ver_avx512_core ? 48 : 80;
        auto PREFETCHSIZEB = 16;

        Zmm regs[] = { zmm8, zmm9, zmm10, zmm11, zmm12, zmm13, zmm14, zmm15,
            zmm16, zmm17, zmm18, zmm19, zmm20, zmm21, zmm22, zmm23, zmm24,
            zmm25, zmm26, zmm27, zmm28, zmm29, zmm30, zmm31 };

        // Function for packing if needed
        auto do_pack = [&](int unroll_m) {
            inLocalLabel();
            mov(BO1, A);
            lea(AO1, ptr[rsp + 128 + OFFSET * SIZE]);
            mov(LL, K);
            sar(LL, 2);
            jle(".pack3", T_NEAR);
            align(16);

            L(".pack2");
            if (!isTransA) {
                for (int i = 0; i < 4; i++) {
                    vmovups(zmm0 | k1, ptr[BO1 + (0 * 16 - OFFSET) * SIZE]);
                    if (unroll_m > 16)
                        vmovups(zmm1 | k2, ptr[BO1 + (1 * 16 - OFFSET) * SIZE]);
                    if (unroll_m > 32)
                        vmovups(zmm2 | k3, ptr[BO1 + (2 * 16 - OFFSET) * SIZE]);
                    add(BO1, LDA);

                    vmovups(ptr[AO1 + (unroll_m * i + 0 * 16 - OFFSET) * SIZE]
                                    | k1,
                            zmm0);
                    if (unroll_m > 16)
                        vmovups(ptr[AO1
                                        + (unroll_m * i + 1 * 16 - OFFSET)
                                                * SIZE]
                                        | k2,
                                zmm1);
                    if (unroll_m > 32)
                        vmovups(ptr[AO1
                                        + (unroll_m * i + 2 * 16 - OFFSET)
                                                * SIZE]
                                        | k3,
                                zmm2);
                }
            } else {
                for (int i = 0; i < 4; i++) {
                    kmovw(k4, k1);
                    vgatherqps(ymm5 | k4,
                            ptr[BO1 + ZSTRIDE + (i - OFFSET) * SIZE]);
                    lea(BO2, ptr[BO1 + LDA * 8]);
                    kshiftrw(k4, k1, 8);
                    vgatherqps(ymm6 | k4,
                            ptr[BO2 + ZSTRIDE + (i - OFFSET) * SIZE]);
                    vshuff64x2(zmm0, zmm5, zmm6, 0x44);

                    if (unroll_m > 16) {
                        lea(BO2, ptr[BO2 + LDA * 8]);
                        kmovw(k4, k2);
                        vgatherqps(ymm5 | k4,
                                ptr[BO2 + ZSTRIDE + (i - OFFSET) * SIZE]);
                        lea(BO2, ptr[BO2 + LDA * 8]);
                        kshiftrw(k4, k2, 8);
                        vgatherqps(ymm6 | k4,
                                ptr[BO2 + ZSTRIDE + (i - OFFSET) * SIZE]);
                        vshuff64x2(zmm1, zmm5, zmm6, 0x44);
                    }

                    if (unroll_m > 32) {
                        lea(BO2, ptr[BO2 + LDA * 8]);
                        kmovw(k4, k3);
                        vgatherqps(ymm5 | k4,
                                ptr[BO2 + ZSTRIDE + (i - OFFSET) * SIZE]);
                        lea(BO2, ptr[BO2 + LDA * 8]);
                        kshiftrw(k4, k3, 8);
                        vgatherqps(ymm6 | k4,
                                ptr[BO2 + ZSTRIDE + (i - OFFSET) * SIZE]);
                        lea(BO2, ptr[BO2 + LDA * 8]);
                        vshuff64x2(zmm2, zmm5, zmm6, 0x44);
                    }

                    vmovups(ptr[AO1 + (unroll_m * i + 0 * 16 - OFFSET) * SIZE],
                            zmm0 | k1);
                    if (unroll_m > 16)
                        vmovups(ptr[AO1
                                        + (unroll_m * i + 1 * 16 - OFFSET)
                                                * SIZE],
                                zmm1 | k2);
                    if (unroll_m > 32)
                        vmovups(ptr[AO1
                                        + (unroll_m * i + 2 * 16 - OFFSET)
                                                * SIZE],
                                zmm2 | k3);
                }
                add(BO1, 4 * SIZE);
            }
            add(AO1, unroll_m * 4 * SIZE);

            sub(LL, 1);
            jg(".pack2", T_NEAR);
            align(16);

            L(".pack3");
            mov(LL, K);
            and_(LL, 3);
            jle(".pack10", T_NEAR);
            align(16);

            L(".pack4");
            if (!isTransA) {
                vmovups(zmm0 | k1, ptr[BO1 + (0 * 16 - OFFSET) * SIZE]);
                if (unroll_m > 16)
                    vmovups(zmm1 | k2, ptr[BO1 + (1 * 16 - OFFSET) * SIZE]);
                if (unroll_m > 32)
                    vmovups(zmm2 | k3, ptr[BO1 + (2 * 16 - OFFSET) * SIZE]);
                add(BO1, LDA);
            } else {
                kmovw(k4, k1);
                vgatherqps(ymm5 | k4, ptr[BO1 + ZSTRIDE + (0 - OFFSET) * SIZE]);
                lea(BO2, ptr[BO1 + LDA * 8]);
                kshiftrw(k4, k1, 8);
                vgatherqps(ymm6 | k4, ptr[BO2 + ZSTRIDE + (0 - OFFSET) * SIZE]);
                vshuff64x2(zmm0, zmm5, zmm6, 0x44);

                if (unroll_m > 16) {
                    lea(BO2, ptr[BO2 + LDA * 8]);
                    kmovw(k4, k2);
                    vgatherqps(ymm5 | k4,
                            ptr[BO2 + ZSTRIDE + (0 - OFFSET) * SIZE]);
                    lea(BO2, ptr[BO2 + LDA * 8]);
                    kshiftrw(k4, k2, 8);
                    vgatherqps(ymm6 | k4,
                            ptr[BO2 + ZSTRIDE + (0 - OFFSET) * SIZE]);
                    vshuff64x2(zmm1, zmm5, zmm6, 0x44);
                }

                if (unroll_m > 32) {
                    lea(BO2, ptr[BO2 + LDA * 8]);
                    kmovw(k4, k3);
                    vgatherqps(ymm5 | k4,
                            ptr[BO2 + ZSTRIDE + (0 - OFFSET) * SIZE]);
                    lea(BO2, ptr[BO2 + LDA * 8]);
                    kshiftrw(k4, k3, 8);
                    vgatherqps(ymm6 | k4,
                            ptr[BO2 + ZSTRIDE + (0 - OFFSET) * SIZE]);
                    lea(BO2, ptr[BO2 + LDA * 8]);
                    vshuff64x2(zmm2, zmm5, zmm6, 0x44);
                }
                add(BO1, SIZE);
            }

            vmovups(ptr[AO1 + (unroll_m * 0 + 0 * 16 - OFFSET) * SIZE],
                    zmm0 | k1);
            if (unroll_m > 16)
                vmovups(ptr[AO1 + (unroll_m * 0 + 1 * 16 - OFFSET) * SIZE],
                        zmm1 | k2);
            if (unroll_m > 32)
                vmovups(ptr[AO1 + (unroll_m * 0 + 2 * 16 - OFFSET) * SIZE],
                        zmm2 | k3);

            add(AO1, unroll_m * SIZE);
            sub(LL, 1);
            jg(".pack4", T_NEAR);
            align(16);

            L(".pack10");
            outLocalLabel();
        };

        // Function to update C, covering masking and other considerations
        auto update = [&](Zmm reg, bool useCO1, int offset, int mask,
                bool useScale = false) {
            vmulps(reg, reg, VALPHA);
            if (!isBeta0) {
                if (!useScale) {
                    switch (mask) {
                    case 0:
                        if (useCO1)
                            vmovups(zmm0, ptr[CO1 + offset * SIZE]);
                        else
                            vmovups(zmm0, ptr[CO2 + offset * SIZE]);
                        break;
                    case 1:
                        if (useCO1)
                            vmovups(zmm0 | k1 | T_z, ptr[CO1 + offset * SIZE]);
                        else
                            vmovups(zmm0 | k1 | T_z, ptr[CO2 + offset * SIZE]);
                        break;
                    case 2:
                        if (useCO1)
                            vmovups(zmm0 | k2 | T_z, ptr[CO1 + offset * SIZE]);
                        else
                            vmovups(zmm0 | k2 | T_z, ptr[CO2 + offset * SIZE]);
                        break;
                    case 3:
                        if (useCO1)
                            vmovups(zmm0 | k3 | T_z, ptr[CO1 + offset * SIZE]);
                        else
                            vmovups(zmm0 | k3 | T_z, ptr[CO2 + offset * SIZE]);
                        break;
                    }
                } else {
                    switch (mask) {
                    case 0:
                        if (useCO1)
                            vmovups(zmm0, ptr[CO1 + LDC + offset * SIZE]);
                        else
                            vmovups(zmm0, ptr[CO2 + LDC + offset * SIZE]);
                        break;
                    case 1:
                        if (useCO1)
                            vmovups(zmm0 | k1 | T_z,
                                    ptr[CO1 + LDC + offset * SIZE]);
                        else
                            vmovups(zmm0 | k1 | T_z,
                                    ptr[CO2 + LDC + offset * SIZE]);
                        break;
                    case 2:
                        if (useCO1)
                            vmovups(zmm0 | k2 | T_z,
                                    ptr[CO1 + LDC + offset * SIZE]);
                        else
                            vmovups(zmm0 | k2 | T_z,
                                    ptr[CO2 + LDC + offset * SIZE]);
                        break;
                    case 3:
                        if (useCO1)
                            vmovups(zmm0 | k3 | T_z,
                                    ptr[CO1 + LDC + offset * SIZE]);
                        else
                            vmovups(zmm0 | k3 | T_z,
                                    ptr[CO2 + LDC + offset * SIZE]);
                        break;
                    }
                }
                if (!isBetaN) {
                    vaddps(zmm0, reg, zmm0);
                } else {
                    vfmadd132ps(zmm0, reg, VBETA);
                }
                if (!useScale) {
                    switch (mask) {
                    case 0:
                        if (useCO1)
                            vmovups(ptr[CO1 + offset * SIZE], zmm0);
                        else
                            vmovups(ptr[CO2 + offset * SIZE], zmm0);
                        break;
                    case 1:
                        if (useCO1)
                            vmovups(ptr[CO1 + offset * SIZE], zmm0 | k1);
                        else
                            vmovups(ptr[CO2 + offset * SIZE], zmm0 | k1);
                        break;
                    case 2:
                        if (useCO1)
                            vmovups(ptr[CO1 + offset * SIZE], zmm0 | k2);
                        else
                            vmovups(ptr[CO2 + offset * SIZE], zmm0 | k2);
                        break;
                    case 3:
                        if (useCO1)
                            vmovups(ptr[CO1 + offset * SIZE], zmm0 | k3);
                        else
                            vmovups(ptr[CO2 + offset * SIZE], zmm0 | k3);
                        break;
                    }
                } else {
                    switch (mask) {
                    case 0:
                        if (useCO1)
                            vmovups(ptr[CO1 + LDC + offset * SIZE], zmm0);
                        else
                            vmovups(ptr[CO2 + LDC + offset * SIZE], zmm0);
                        break;
                    case 1:
                        if (useCO1)
                            vmovups(ptr[CO1 + LDC + offset * SIZE], zmm0 | k1);
                        else
                            vmovups(ptr[CO2 + LDC + offset * SIZE], zmm0 | k1);
                        break;
                    case 2:
                        if (useCO1)
                            vmovups(ptr[CO1 + LDC + offset * SIZE], zmm0 | k2);
                        else
                            vmovups(ptr[CO2 + LDC + offset * SIZE], zmm0 | k2);
                        break;
                    case 3:
                        if (useCO1)
                            vmovups(ptr[CO1 + LDC + offset * SIZE], zmm0 | k3);
                        else
                            vmovups(ptr[CO2 + LDC + offset * SIZE], zmm0 | k3);
                        break;
                    }
                }
            } else {
                if (!useScale) {
                    switch (mask) {
                    case 0:
                        if (useCO1)
                            vmovups(ptr[CO1 + offset * SIZE], reg);
                        else
                            vmovups(ptr[CO2 + offset * SIZE], reg);
                        break;
                    case 1:
                        if (useCO1)
                            vmovups(ptr[CO1 + offset * SIZE], reg | k1);
                        else
                            vmovups(ptr[CO2 + offset * SIZE], reg | k1);
                        break;
                    case 2:
                        if (useCO1)
                            vmovups(ptr[CO1 + offset * SIZE], reg | k2);
                        else
                            vmovups(ptr[CO2 + offset * SIZE], reg | k2);
                        break;
                    case 3:
                        if (useCO1)
                            vmovups(ptr[CO1 + offset * SIZE], reg | k3);
                        else
                            vmovups(ptr[CO2 + offset * SIZE], reg | k3);
                        break;
                    }
                } else {
                    switch (mask) {
                    case 0:
                        if (useCO1)
                            vmovups(ptr[CO1 + LDC + offset * SIZE], reg);
                        else
                            vmovups(ptr[CO2 + LDC + offset * SIZE], reg);
                        break;
                    case 1:
                        if (useCO1)
                            vmovups(ptr[CO1 + LDC + offset * SIZE], reg | k1);
                        else
                            vmovups(ptr[CO2 + LDC + offset * SIZE], reg | k1);
                        break;
                    case 2:
                        if (useCO1)
                            vmovups(ptr[CO1 + LDC + offset * SIZE], reg | k2);
                        else
                            vmovups(ptr[CO2 + LDC + offset * SIZE], reg | k2);
                        break;
                    case 3:
                        if (useCO1)
                            vmovups(ptr[CO1 + LDC + offset * SIZE], reg | k3);
                        else
                            vmovups(ptr[CO2 + LDC + offset * SIZE], reg | k3);
                        break;
                    }
                }
            }
            vpxorq(reg, reg, reg);
        };

        // Loop with unroll_n - 2 FMAs; called by innerkernel
        auto fmaloop = [&](int unroll_m, int unroll_n, int iteration) {
            for (int i = 2; i < unroll_n; i++) {
                if (ver == ver_avx512_core) {
                    if (!isTransB) {
                        switch (i) {
                        case 2:
                            vbroadcastss(
                                    zmm3,
                                    ptr[BO1 + LDB * 2
                                            + (iteration - OFFSET) * SIZE]);
                            break;
                        case 3:
                            vbroadcastss(
                                    zmm3,
                                    ptr[BO1 + LDB3
                                            + (iteration - OFFSET) * SIZE]);
                            break;
                        case 4:
                            vbroadcastss(zmm3,
                                    ptr[BO2 + (iteration - OFFSET) * SIZE]);
                            break;
                        case 5:
                            vbroadcastss(
                                    zmm3,
                                    ptr[BO2 + LDB * 1
                                            + (iteration - OFFSET) * SIZE]);
                            break;
                        case 6:
                            vbroadcastss(
                                    zmm3,
                                    ptr[BO2 + LDB * 2
                                            + (iteration - OFFSET) * SIZE]);
                            break;
                        case 7:
                            vbroadcastss(
                                    zmm3,
                                    ptr[BO2 + LDB3
                                            + (iteration - OFFSET) * SIZE]);
                            break;
                        }
                    } else {
                        vbroadcastss(zmm3, ptr[BO1 + (i - OFFSET) * SIZE]);
                    }
                    vfmadd231ps(regs[i], zmm3, zmm0);
                    if (unroll_m >= 32)
                        vfmadd231ps(regs[i + 8], zmm3, zmm1);
                    if (unroll_m >= 48)
                        vfmadd231ps(regs[i + 16], zmm3, zmm2);
                } else {
                    if (!isTransB) {
                        switch (i) {
                        case 2:
                            vfmadd231ps(regs[i], zmm0,
                                    zword_b[BO1 + LDB * 2
                                    + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 32)
                                vfmadd231ps(regs[i + 8], zmm1,
                                        zword_b[BO1 + LDB * 2
                                        + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 48)
                                vfmadd231ps(regs[i + 16], zmm2,
                                        zword_b[BO1 + LDB * 2
                                        + (iteration - OFFSET) * SIZE]);
                            break;
                        case 3:
                            vfmadd231ps(regs[i], zmm0,
                                    zword_b[BO1 + LDB3
                                    + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 32)
                                vfmadd231ps(regs[i + 8], zmm1,
                                        zword_b[BO1 + LDB3
                                        + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 48)
                                vfmadd231ps(regs[i + 16], zmm2,
                                        zword_b[BO1 + LDB3
                                        + (iteration - OFFSET) * SIZE]);
                            break;
                        case 4:
                            vfmadd231ps(regs[i], zmm0,
                                    zword_b[BO2 + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 32)
                                vfmadd231ps(regs[i + 8], zmm1,
                                        zword_b[BO2 + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 48)
                                vfmadd231ps(regs[i + 16], zmm2,
                                        zword_b[BO2 + (iteration - OFFSET) * SIZE]);
                            break;
                        case 5:
                            vfmadd231ps(regs[i], zmm0,
                                    zword_b[BO2 + LDB * 1
                                    + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 32)
                                vfmadd231ps(regs[i + 8], zmm1,
                                        zword_b[BO2 + LDB * 1
                                        + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 48)
                                vfmadd231ps(regs[i + 16], zmm2,
                                        zword_b[BO2 + LDB * 1
                                        + (iteration - OFFSET) * SIZE]);
                            break;
                        case 6:
                            vfmadd231ps(regs[i], zmm0,
                                    zword_b[BO2 + LDB * 2
                                    + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 32)
                                vfmadd231ps(regs[i + 8], zmm1,
                                        zword_b[BO2 + LDB * 2
                                        + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 48)
                                vfmadd231ps(regs[i + 16], zmm2,
                                        zword_b[BO2 + LDB * 2
                                        + (iteration - OFFSET) * SIZE]);
                            break;
                        case 7:
                            vfmadd231ps(regs[i], zmm0,
                                    zword_b[BO2 + LDB3
                                    + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 32)
                                vfmadd231ps(regs[i + 8], zmm1,
                                        zword_b[BO2 + LDB3
                                        + (iteration - OFFSET) * SIZE]);
                            if (unroll_m >= 48)
                                vfmadd231ps(regs[i + 16], zmm2,
                                        zword_b[BO2 + LDB3
                                        + (iteration - OFFSET) * SIZE]);
                            break;
                        }
                    } else {
                        vfmadd231ps(
                                regs[i], zmm0, zword_b[BO1 + (i - OFFSET) * SIZE]);
                        if (unroll_m >= 32)
                            vfmadd231ps(regs[i + 8], zmm1,
                                    zword_b[BO1 + (i - OFFSET) * SIZE]);
                        if (unroll_m >= 48)
                            vfmadd231ps(regs[i + 16], zmm2,
                                    zword_b[BO1 + (i - OFFSET) * SIZE]);
                    }
                }
            }
        };

        // Innerkernel; called by kernel
        auto innerkernel = [&](int unroll_m, int unroll_n, bool isDirect,
                bool isCopy, bool doCPrefetch, bool isUnmasked = true) {
            inLocalLabel();

            for (int i = 0; i < 8; i++) {
                if (!isDirect) {
                    prefetcht0(ptr[AO1
                            + (PREFETCHSIZEA + i * unroll_m + 0 * 16 - OFFSET)
                                    * SIZE]);
                    if (unroll_m >= 32)
                        prefetcht0(ptr[AO1
                            + (PREFETCHSIZEA + i * unroll_m + 1 * 16 - OFFSET)
                                    * SIZE]);
                    if (unroll_m >= 48)
                        prefetcht0(ptr[AO1
                            + (PREFETCHSIZEA + i * unroll_m + 2 * 16 - OFFSET)
                                    * SIZE]);
                } else {
                    prefetcht0(ptr[AO1 + LDA4 + (16 * 0 * SIZE)]);
                    if (unroll_m >= 32)
                        prefetcht0(ptr[AO1 + LDA4 + (16 * 1 * SIZE)]);
                    if (unroll_m >= 48)
                        prefetcht0(ptr[AO1 + LDA4 + (16 * 2 * SIZE)]);
                }

                if (!isDirect) {
                    if (i != 0) {
                        if (isUnmasked || unroll_m > 16) {
                            vmovups(zmm0,
                                    ptr[AO1
                                            + (unroll_m * i + 0 * 16 - OFFSET)
                                                    * SIZE]);
                        } else {
                            vmovups(zmm0 | k1 | T_z,
                                    ptr[AO1
                                            + (unroll_m * i + 0 * 16 - OFFSET)
                                                    * SIZE]);
                        }
                        if (unroll_m >= 32) {
                            if (isUnmasked || unroll_m > 32) {
                                vmovups(zmm1, ptr[AO1
                                                      + (unroll_m * i + 1 * 16
                                                                - OFFSET)
                                                              * SIZE]);
                            } else {
                                vmovups(zmm1 | k2 | T_z,
                                        ptr[AO1
                                                + (unroll_m * i + 1 * 16
                                                          - OFFSET)
                                                        * SIZE]);
                            }
                        }
                        if (unroll_m >= 48) {
                            if (isUnmasked) {
                                vmovups(zmm2, ptr[AO1
                                                      + (unroll_m * i + 2 * 16
                                                                - OFFSET)
                                                              * SIZE]);
                            } else {
                                vmovups(zmm2 | k3 | T_z,
                                        ptr[AO1
                                                + (unroll_m * i + 2 * 16
                                                          - OFFSET)
                                                        * SIZE]);
                            }
                        }
                    }
                } else {
                    if (isUnmasked || unroll_m > 16) {
                        vmovups(zmm0, ptr[AO1 + (0 * 16 - OFFSET) * SIZE]);
                    } else {
                        vmovups(zmm0 | k1 | T_z,
                                ptr[AO1 + (0 * 16 - OFFSET) * SIZE]);
                    }
                    if (unroll_m >= 32) {
                        if (isUnmasked || unroll_m > 32) {
                            vmovups(zmm1, ptr[AO1 + (1 * 16 - OFFSET) * SIZE]);
                        } else {
                            vmovups(zmm1 | k2 | T_z,
                                    ptr[AO1 + (1 * 16 - OFFSET) * SIZE]);
                        }
                    }
                    if (unroll_m >= 48) {
                        if (isUnmasked) {
                            vmovups(zmm2, ptr[AO1 + (2 * 16 - OFFSET) * SIZE]);
                        } else {
                            vmovups(zmm2 | k3 | T_z,
                                    ptr[AO1 + (2 * 16 - OFFSET) * SIZE]);
                        }
                    }
                    add(AO1, LDA);
                }

                if (ver == ver_avx512_core) {
                    if (!isTransB) {
                        vbroadcastss(zmm3, ptr[BO1 + (i - OFFSET) * SIZE]);
                    } else {
                        vbroadcastss(zmm3, ptr[BO1 + (0 - OFFSET) * SIZE]);
                    }
                    vfmadd231ps(regs[0], zmm3, zmm0);
                    if (unroll_m >= 32)
                        vfmadd231ps(regs[0 + 8], zmm3, zmm1);
                    if (unroll_m >= 48)
                        vfmadd231ps(regs[0 + 16], zmm3, zmm2);
                } else {
                    if (!isTransB) {
                        vfmadd231ps(regs[0], zmm0,
                                zword_b[BO1 + (i - OFFSET) * SIZE]);
                        if (unroll_m >= 32)
                            vfmadd231ps(regs[0 + 8], zmm1,
                                    zword_b[BO1 + (i - OFFSET) * SIZE]);
                        if (unroll_m >= 48)
                            vfmadd231ps(regs[0 + 16], zmm2,
                                    zword_b[BO1 + (i - OFFSET) * SIZE]);
                    } else {
                        vfmadd231ps(regs[0], zmm0,
                                zword_b[BO1 + (0 - OFFSET) * SIZE]);
                        if (unroll_m >= 32)
                            vfmadd231ps(regs[0 + 8], zmm1,
                                    zword_b[BO1 + (0 - OFFSET) * SIZE]);
                        if (unroll_m >= 48)
                            vfmadd231ps(regs[0 + 16], zmm2,
                                    zword_b[BO1 + (0 - OFFSET) * SIZE]);
                    }
                }

                if (unroll_n >= i + 1) {
                    if (!isTransB) {
                        switch (i) {
                        case 0:
                            prefetcht0(
                                    ptr[BO1 + (PREFETCHSIZEB - OFFSET) * SIZE]);
                            break;
                        case 1:
                            prefetcht0(ptr[BO1 + LDB
                                    + (PREFETCHSIZEB - OFFSET) * SIZE]);
                            break;
                        case 2:
                            prefetcht0(ptr[BO1 + LDB * 2
                                    + (PREFETCHSIZEB - OFFSET) * SIZE]);
                            break;
                        case 3:
                            prefetcht0(ptr[BO1 + LDB3
                                    + (PREFETCHSIZEB - OFFSET) * SIZE]);
                            break;
                        case 4:
                            prefetcht0(
                                    ptr[BO2 + (PREFETCHSIZEB - OFFSET) * SIZE]);
                            break;
                        case 5:
                            prefetcht0(ptr[BO2 + LDB
                                    + (PREFETCHSIZEB - OFFSET) * SIZE]);
                            break;
                        case 6:
                            prefetcht0(ptr[BO2 + LDB * 2
                                    + (PREFETCHSIZEB - OFFSET) * SIZE]);
                            break;
                        case 7:
                            prefetcht0(ptr[BO2 + LDB3
                                    + (PREFETCHSIZEB - OFFSET) * SIZE]);
                            break;
                        }
                    }
                }

                if (unroll_n >= 2) {
                    if (ver == ver_avx512_core) {
                        if (!isTransB) {
                            vbroadcastss(zmm3,
                                    ptr[BO1 + LDB * 1 + (i - OFFSET) * SIZE]);
                        } else {
                            vbroadcastss(zmm3, ptr[BO1 + (1 - OFFSET) * SIZE]);
                        }
                        vfmadd231ps(regs[1], zmm3, zmm0);
                        if (unroll_m >= 32)
                            vfmadd231ps(regs[1 + 8], zmm3, zmm1);
                        if (unroll_m >= 48)
                            vfmadd231ps(regs[1 + 16], zmm3, zmm2);
                    } else {
                        if (!isTransB) {
                            vfmadd231ps(regs[1], zmm0,
                                    zword_b[BO1 + LDB * 1 + (i - OFFSET) * SIZE]);
                            if (unroll_m >= 32)
                                vfmadd231ps(regs[1 + 8], zmm1,
                                        zword_b[BO1 + LDB * 1
                                        + (i - OFFSET) * SIZE]);
                            if (unroll_m >= 48)
                                vfmadd231ps(regs[1 + 16], zmm2,
                                        zword_b[BO1 + LDB * 1
                                        + (i - OFFSET) * SIZE]);
                        } else {
                            vfmadd231ps(regs[1], zmm0,
                                    zword_b[BO1 + (1 - OFFSET) * SIZE]);
                            if (unroll_m >= 32)
                                vfmadd231ps(regs[1 + 8], zmm1,
                                        zword_b[BO1 + (1 - OFFSET) * SIZE]);
                            if (unroll_m >= 48)
                                vfmadd231ps(regs[1 + 16], zmm2,
                                        zword_b[BO1 + (1 - OFFSET) * SIZE]);
                        }
                    }
                }

                if (isCopy) {
                    if (isUnmasked || unroll_m > 16) {
                        vmovups(ptr[LDA4
                                        + (unroll_m * i + 0 * 16 - OFFSET)
                                                * SIZE],
                                zmm0);
                    } else {
                        vmovups(ptr[LDA4
                                        + (unroll_m * i + 0 * 16 - OFFSET)
                                                * SIZE],
                                zmm0 | k1);
                    }
                    if (unroll_m >= 32) {
                        if (isUnmasked || unroll_m > 32) {
                            vmovups(ptr[LDA4
                                            + (unroll_m * i + 1 * 16 - OFFSET)
                                                    * SIZE],
                                    zmm1);
                        } else {
                            vmovups(ptr[LDA4
                                            + (unroll_m * i + 1 * 16 - OFFSET)
                                                    * SIZE],
                                    zmm1 | k2);
                        }
                    }
                    if (unroll_m >= 48) {
                        if (isUnmasked) {
                            vmovups(ptr[LDA4
                                            + (unroll_m * i + 2 * 16 - OFFSET)
                                                    * SIZE],
                                    zmm2);
                        } else {
                            vmovups(ptr[LDA4
                                            + (unroll_m * i + 2 * 16 - OFFSET)
                                                    * SIZE],
                                    zmm2 | k3);
                        }
                    }
                    if (i == 7)
                        sub(LDA4, -unroll_m * 8 * SIZE);
                }
                fmaloop(unroll_m, unroll_n, i);

                if (i == 1) {
                    if (doCPrefetch) {
                        if (ver == ver_avx512_core)
                            prefetchw(ptr[CO2 + 0 * 16 * SIZE]);
                        else
                            prefetcht0(ptr[CO2 + 0 * 16 * SIZE]);
                    }
                }
                if (i == 3) {
                    if (doCPrefetch && unroll_m >= 32) {
                        if (ver == ver_avx512_core)
                            prefetchw(ptr[CO2 + 1 * 16 * SIZE]);
                        else
                            prefetcht0(ptr[CO2 + 1 * 16 * SIZE]);
                    }
                    if (!isTransA) {
                        if (ver == ver_avx512_core)
                            prefetcht0(ptr[AA + 16 * 0 * SIZE]);
                        else
                            prefetcht2(ptr[AA + 16 * 0 * SIZE]);
                    }
                }
                if (i == 5) {
                    if (doCPrefetch) {
                        if (unroll_m >= 48) {
                            if (ver == ver_avx512_core)
                                prefetchw(ptr[CO2 + 2 * 16 * SIZE]);
                            else
                                prefetcht0(ptr[CO2 + 2 * 16 * SIZE]);
                        }
                        add(CO2, LDC);
                    }
                    if (!isTransA) {
                        if (unroll_m >= 32) {
                            if (ver == ver_avx512_core)
                                prefetcht0(ptr[AA + 16 * 1 * SIZE]);
                            else
                                prefetcht2(ptr[AA + 16 * 1 * SIZE]);
                        }
                    }
                }

                if (isTransB) {
                    prefetcht0(ptr[BO1 + BO2]);
                    add(BO1, LDB);
                }
            } // end of for loop

            if (!isTransB) {
                sub(BO1, -8 * SIZE);
                if (unroll_n >= 4)
                    sub(BO2, -8 * SIZE);
            }
            if (!isTransA) {
                if (unroll_m >= 48) {
                    if (ver == ver_avx512_core)
                        prefetcht0(ptr[AA + 16 * 2 * SIZE]);
                    else
                        prefetcht2(ptr[AA + 16 * 2 * SIZE]);
                }
                lea(AA, ptr[AA + LDA]);
            }

            if (!isDirect) {
                if (isUnmasked || unroll_m > 16) {
                    vmovups(zmm0,
                            ptr[AO1 + (unroll_m * 8 + 0 * 16 - OFFSET) * SIZE]);
                } else {
                    vmovups(zmm0 | k1 | T_z,
                            ptr[AO1 + (unroll_m * 8 + 0 * 16 - OFFSET) * SIZE]);
                }
                if (unroll_m >= 32) {
                    if (isUnmasked || unroll_m > 32) {
                        vmovups(zmm1, ptr[AO1
                                              + (unroll_m * 8 + 1 * 16 - OFFSET)
                                                      * SIZE]);
                    } else {
                        vmovups(zmm1 | k2 | T_z,
                                ptr[AO1
                                        + (unroll_m * 8 + 1 * 16 - OFFSET)
                                                * SIZE]);
                    }
                }
                if (unroll_m >= 48) {
                    if (isUnmasked) {
                        vmovups(zmm2, ptr[AO1
                                              + (unroll_m * 8 + 2 * 16 - OFFSET)
                                                      * SIZE]);
                    } else {
                        vmovups(zmm2 | k3 | T_z,
                                ptr[AO1
                                        + (unroll_m * 8 + 2 * 16 - OFFSET)
                                                * SIZE]);
                    }
                }
                sub(AO1, -unroll_m * 8 * SIZE);
            }

            sub(LL, 1);
            outLocalLabel();
        };

        // Main kernel; does prefetching and calls innerkernel
        // After calculating results in registers, writes back to C matrix by
        // calling update
        auto kernel = [&](int unroll_m, int unroll_n, bool isDirect,
                bool isCopy, bool isUnmasked = true) {
            inLocalLabel();
            if (!isDirect) {
                lea(AO1, ptr[rsp + 128 + OFFSET * SIZE]);
            } else {
                mov(AO1, A);
            }

            if (isCopy) {
                lea(LDA4, ptr[rsp + 128 + OFFSET * SIZE]);
            } else {
                auto step = ver == ver_avx512_core ? 2 : 4;
                lea(LDA4, ptr[LDA * step + (16 - 1 - OFFSET) * SIZE]);
            }

            if (isTransB) {
                lea(BO2, ptr[LDB * 4 + (16 / 2 - 1 - OFFSET) * SIZE]);
            }

            if (!isDirect) {
                if (isUnmasked || unroll_m > 16) {
                    vmovups(zmm0,
                            ptr[AO1 + (unroll_m * 0 + 0 * 16 - OFFSET) * SIZE]);
                } else {
                    vmovups(zmm0 | k1 | T_z,
                            ptr[AO1 + (unroll_m * 0 + 0 * 16 - OFFSET) * SIZE]);
                }
                if (unroll_m >= 32) {
                    if (isUnmasked || unroll_m > 32) {
                        vmovups(zmm1, ptr[AO1
                                              + (unroll_m * 0 + 1 * 16 - OFFSET)
                                                      * SIZE]);
                    } else {
                        vmovups(zmm1 | k2 | T_z,
                                ptr[AO1
                                        + (unroll_m * 0 + 1 * 16 - OFFSET)
                                                * SIZE]);
                    }
                }
                if (unroll_m >= 48) {
                    if (isUnmasked) {
                        vmovups(zmm2, ptr[AO1
                                              + (unroll_m * 0 + 2 * 16 - OFFSET)
                                                      * SIZE]);
                    } else {
                        vmovups(zmm2 | k3 | T_z,
                                ptr[AO1
                                        + (unroll_m * 0 + 2 * 16 - OFFSET)
                                                * SIZE]);
                    }
                }
            }

            mov(LL, K);
            sar(LL, 3);
            sub(LL, SECOND_FETCH);
            jle(".kernel13", T_NEAR);
            align(16);

            L(".kernel12");
            innerkernel(
                    unroll_m, unroll_n, isDirect, isCopy, false, isUnmasked);
            jg(".kernel12", T_NEAR);
            align(16);

            L(".kernel13");
            lea(CO2, ptr[CO1 + (16 - 1) * SIZE]);
            add(LL, unroll_n);
            jle(".kernel15", T_NEAR);
            align(16);

            L(".kernel14");
            innerkernel(unroll_m, unroll_n, isDirect, isCopy, true, isUnmasked);
            jg(".kernel14", T_NEAR);
            align(16);

            L(".kernel15");
            mov(LL, K);
            and_(LL, 7);
            jle(".kernel18", T_NEAR);
            align(16);

            L(".kernel16");
            if (isDirect) {
                if (isUnmasked || unroll_m > 16) {
                    vmovups(zmm0, ptr[AO1 + (0 * 16 - OFFSET) * SIZE]);
                } else {
                    vmovups(zmm0 | k1 | T_z,
                            ptr[AO1 + (0 * 16 - OFFSET) * SIZE]);
                }
                if (unroll_m >= 32) {
                    if (isUnmasked || unroll_m > 32) {
                        vmovups(zmm1, ptr[AO1 + (1 * 16 - OFFSET) * SIZE]);
                    } else {
                        vmovups(zmm1 | k2 | T_z,
                                ptr[AO1 + (1 * 16 - OFFSET) * SIZE]);
                    }
                }
                if (unroll_m >= 48) {
                    if (isUnmasked) {
                        vmovups(zmm2, ptr[AO1 + (2 * 16 - OFFSET) * SIZE]);
                    } else {
                        vmovups(zmm2 | k3 | T_z,
                                ptr[AO1 + (2 * 16 - OFFSET) * SIZE]);
                    }
                }
                add(AO1, LDA);
            }

            for (int i = 0; i < unroll_n; i++) {
                if (!isTransB) {
                    switch (i) {
                    case 0:
                        vbroadcastss(zmm3, ptr[BO1 + (0 - OFFSET) * SIZE]);
                        break;
                    case 1:
                        vbroadcastss(
                                zmm3, ptr[BO1 + LDB * 1 + (0 - OFFSET) * SIZE]);
                        break;
                    case 2:
                        vbroadcastss(
                                zmm3, ptr[BO1 + LDB * 2 + (0 - OFFSET) * SIZE]);
                        break;
                    case 3:
                        vbroadcastss(
                                zmm3, ptr[BO1 + LDB3 + (0 - OFFSET) * SIZE]);
                        break;
                    case 4:
                        vbroadcastss(zmm3, ptr[BO2 + (0 - OFFSET) * SIZE]);
                        break;
                    case 5:
                        vbroadcastss(
                                zmm3, ptr[BO2 + LDB * 1 + (0 - OFFSET) * SIZE]);
                        break;
                    case 6:
                        vbroadcastss(
                                zmm3, ptr[BO2 + LDB * 2 + (0 - OFFSET) * SIZE]);
                        break;
                    case 7:
                        vbroadcastss(
                                zmm3, ptr[BO2 + LDB3 + (0 - OFFSET) * SIZE]);
                        break;
                    }
                } else {
                    vbroadcastss(zmm3, ptr[BO1 + (i - OFFSET) * SIZE]);
                }
                vfmadd231ps(regs[i], zmm3, zmm0);
                if (unroll_m >= 32) {
                    vfmadd231ps(regs[i + 8], zmm3, zmm1);
                }
                if (unroll_m >= 48) {
                    vfmadd231ps(regs[i + 16], zmm3, zmm2);
                }
            }

            if (isCopy) {
                if (isUnmasked || unroll_m > 16) {
                    vmovups(ptr[LDA4 + (unroll_m * 0 + 0 * 16 - OFFSET) * SIZE],
                            zmm0);
                } else {
                    vmovups(ptr[LDA4 + (unroll_m * 0 + 0 * 16 - OFFSET) * SIZE],
                            zmm0 | k1);
                }
                if (unroll_m >= 32) {
                    if (isUnmasked || unroll_m > 32) {
                        vmovups(ptr[LDA4
                                        + (unroll_m * 0 + 1 * 16 - OFFSET)
                                                * SIZE],
                                zmm1);
                    } else {
                        vmovups(ptr[LDA4
                                        + (unroll_m * 0 + 1 * 16 - OFFSET)
                                                * SIZE],
                                zmm1 | k2);
                    }
                }
                if (unroll_m >= 48) {
                    if (isUnmasked) {
                        vmovups(ptr[LDA4
                                        + (unroll_m * 0 + 2 * 16 - OFFSET)
                                                * SIZE],
                                zmm2);
                    } else {
                        vmovups(ptr[LDA4
                                        + (unroll_m * 0 + 2 * 16 - OFFSET)
                                                * SIZE],
                                zmm2 | k3);
                    }
                }
                sub(LDA4, -unroll_m * SIZE);
            }

            if (!isDirect) {
                if (isUnmasked || unroll_m > 16) {
                    vmovups(zmm0,
                            ptr[AO1 + (unroll_m * 1 + 0 * 16 - OFFSET) * SIZE]);
                } else {
                    vmovups(zmm0 | k1 | T_z,
                            ptr[AO1 + (unroll_m * 1 + 0 * 16 - OFFSET) * SIZE]);
                }
                if (unroll_m >= 32) {
                    if (isUnmasked || unroll_m > 32) {
                        vmovups(zmm1, ptr[AO1
                                              + (unroll_m * 1 + 1 * 16 - OFFSET)
                                                      * SIZE]);
                    } else {
                        vmovups(zmm1 | k2 | T_z,
                                ptr[AO1
                                        + (unroll_m * 1 + 1 * 16 - OFFSET)
                                                * SIZE]);
                    }
                }
                if (unroll_m >= 48) {
                    if (isUnmasked) {
                        vmovups(zmm2, ptr[AO1
                                              + (unroll_m * 1 + 2 * 16 - OFFSET)
                                                      * SIZE]);
                    } else {
                        vmovups(zmm2 | k3 | T_z,
                                ptr[AO1
                                        + (unroll_m * 1 + 2 * 16 - OFFSET)
                                                * SIZE]);
                    }
                }
                sub(AO1, -unroll_m * SIZE);
            }

            if (!isTransB) {
                sub(BO1, -SIZE);
                if (unroll_n >= 4) {
                    sub(BO2, -SIZE);
                }
            } else {
                add(BO1, LDB);
            }

            sub(LL, 1);
            jg(".kernel16", T_NEAR);
            align(16);

            L(".kernel18");
            vbroadcastss(VALPHA, ALPHA);

            if (isBetaN) {
                vbroadcastss(VBETA, BETA);
            }

            // Write back the results; all beta cases need to be handled
            if (hasBias) {
                mov(BIAS1, BIAS);
                if (isUnmasked || unroll_m > 16)
                    vmovups(VBIAS1, ptr[BIAS1 + 0 * SIZE]);
                else
                    vmovups(VBIAS1 | k1 | T_z, ptr[BIAS1 + 0 * SIZE]);
                if (unroll_m >= 32) {
                    if (isUnmasked || unroll_m > 32)
                        vmovups(VBIAS2, ptr[BIAS1 + 16 * SIZE]);
                    else
                        vmovups(VBIAS2 | k2 | T_z, ptr[BIAS1 + 16 * SIZE]);
                }
                if (unroll_m >= 48) {
                    if (isUnmasked)
                        vmovups(VBIAS3, ptr[BIAS1 + 32 * SIZE]);
                    else
                        vmovups(VBIAS3 | k3 | T_z, ptr[BIAS1 + 32 * SIZE]);
                }
            }

            for (int i = 0; i < unroll_n; i++) {
                bool useScale = i % 2 != 0;
                bool useCO1 = i < 2;
                if (i == 2)
                    lea(CO2, ptr[CO1 + LDC * 2]);
                if (i == 4 || i == 6)
                    lea(CO2, ptr[CO2 + LDC * 2]);
                if (hasBias)
                    vaddps(regs[i], VBIAS1, regs[i]);
                if (isUnmasked || unroll_m > 16) {
                    update(regs[i], useCO1, 0, 0, useScale);
                } else {
                    update(regs[i], useCO1, 0, 1, useScale);
                }
                if (unroll_m >= 32) {
                    if (hasBias)
                        vaddps(regs[i + 8], VBIAS2, regs[i + 8]);
                    if (isUnmasked || unroll_m > 32) {
                        update(regs[i + 8], useCO1, 16, 0, useScale);
                    } else {
                        update(regs[i + 8], useCO1, 16, 2, useScale);
                    }
                }
                if (unroll_m >= 48) {
                    if (hasBias)
                        vaddps(regs[i + 16], VBIAS3, regs[i + 16]);
                    if (isUnmasked) {
                        update(regs[i + 16], useCO1, 32, 0, useScale);
                    } else {
                        update(regs[i + 16], useCO1, 32, 3, useScale);
                    }
                }
            }

            switch (unroll_n) {
            case 1: add(CO1, LDC); break;
            case 2: lea(CO1, ptr[CO1 + LDC * 2]); break;
            case 3: lea(CO1, ptr[CO2 + LDC * 1]); break;
            case 4: lea(CO1, ptr[CO2 + LDC * 2]); break;
            case 5: lea(CO1, ptr[CO2 + LDC * 1]); break;
            case 6: lea(CO1, ptr[CO2 + LDC * 2]); break;
            case 7: lea(CO1, ptr[CO2 + LDC * 1]); break;
            case 8: lea(CO1, ptr[CO2 + LDC * 2]); break;
            }

            // Compute next address of B
            if (!isTransB) {
                lea(rax, ptr[K * SIZE]);
                switch (unroll_n) {
                case 1:
                    add(BO1, LDB);
                    add(BO2, LDB);
                    break;
                case 2:
                    lea(BO1, ptr[BO1 + LDB * 2]);
                    lea(BO2, ptr[BO2 + LDB * 2]);
                    break;
                case 3:
                    lea(BO1, ptr[BO1 + LDB3]);
                    lea(BO2, ptr[BO2 + LDB3]);
                    break;
                case 4:
                    lea(BO1, ptr[BO1 + LDB * 4]);
                    lea(BO2, ptr[BO2 + LDB * 4]);
                    break;
                case 5:
                    lea(BO1, ptr[BO1 + LDB * 4]);
                    add(BO1, LDB);
                    lea(BO2, ptr[BO2 + LDB * 4]);
                    add(BO2, LDB);
                    break;
                case 6:
                    lea(BO1, ptr[BO1 + LDB3 * 2]);
                    lea(BO2, ptr[BO2 + LDB3 * 2]);
                    break;
                case 7:
                    lea(BO1, ptr[BO1 + LDB * 8]);
                    sub(BO1, LDB);
                    lea(BO2, ptr[BO2 + LDB * 8]);
                    sub(BO2, LDB);
                    break;
                case 8:
                    lea(BO1, ptr[BO1 + LDB * 8]);
                    lea(BO2, ptr[BO2 + LDB * 8]);
                    break;
                }
                sub(BO1, rax);
                sub(BO2, rax);
            } else {
                mov(rax, LDB);
                imul(rax, K);
                sub(BO1, rax);
                add(BO1, unroll_n * SIZE);
            }

            outLocalLabel();
        };

        // High-level subroutine; does packing if needed, then splits C matrix.
        // Operates on chunks of 48 rows, 8 columns at a time (handling tail
        // cases appropriately by doing 32 or 16 rows, and/or with masking,
        // and/or fewer columns).
        auto subloop = [&](int unroll_m) {
            inLocalLabel();

            Label l_subloop_20x[8], l_subloop_mask_20x[8];
            Label l_subloop_30x[8], l_subloop_mask_30x[8];

            // Create mask
            mov(BO1, rcx);
            mov(rcx, M);
            sub(rcx, unroll_m - 16);
            mov(CO1, 16);
            cmp(rcx, 16);

            cmovg(rcx, CO1);
            mov(rax, 1);
            sal(rax, cl);
            sub(rax, 1);
            mov(rcx, 0xffff);

            if (unroll_m == 16) {
                kmovw(k1, eax);
            } else if (unroll_m == 32) {
                kmovw(k1, ecx);
                kmovw(k2, eax);
            } else {
                kmovw(k1, ecx);
                kmovw(k2, ecx);
                kmovw(k3, eax);
            }
            mov(rcx, BO1);

            and_(rax, 0xffff);
            cmp(rax, 0xffff);
            jne(".subloop96", T_NEAR);

            if (isTransA) {
                do_pack(unroll_m);
            }

            mov(CO1, C);
            add(C, unroll_m * SIZE);

            mov(BO1, B);
            if (!isTransB) {
                lea(BO2, ptr[B + LDB * 4]);
            }

            if (!isTransA) {
                lea(AA, ptr[A + (unroll_m + 16 - 1 - OFFSET) * SIZE]);
                cmp(M, UNROLL_M);
                jg(".subloop98", T_NEAR);

                mov(AA, ORIG_A);
                lea(AA, ptr[AA + (16 - 1 - OFFSET) * SIZE]);
                L(".subloop98");
            }

            mov(LL, N);
            mov(I, LL);
            if (!isTransA) {
                // If N is too small, skip copy operation
                cmp(LL, UNROLL_N * 3);
                jle(".subloop30", T_NEAR);

                // If A is not aligned to cache line
                cmp(FLAG, 0);
                je(".subloop30", T_NEAR);
            } else {
                cmp(LL, UNROLL_N);
                jl(l_subloop_20x[1], T_NEAR);
            }
            align(16);

            if (!isTransA) {
                kernel(unroll_m, UNROLL_N, true, true);
            } else {
                kernel(unroll_m, UNROLL_N, false, false);
            }

            sub(I, UNROLL_N);
            cmp(I, UNROLL_N);
            jl(l_subloop_20x[1], T_NEAR);
            align(16);

            L(".subloop11");
            kernel(unroll_m, UNROLL_N, false, false);
            sub(I, UNROLL_N);
            cmp(I, UNROLL_N);
            jge(".subloop11", T_NEAR);
            align(16);

            for (int i = 1; i <= 7; i++) {
                L(l_subloop_20x[i]);
                cmp(I, i);
                if (i < 7) {
                    jne(l_subloop_20x[i + 1], T_NEAR);
                } else {
                    jne(".subloop99", T_NEAR);
                }
                kernel(unroll_m, i, false, false);
                jmp(".subloop99", T_NEAR);
                align(16);
            }

            if (!isTransA) {
                L(".subloop30");
                cmp(I, UNROLL_N);
                jl(l_subloop_30x[1], T_NEAR);
                align(16);

                L(".subloop31");
                kernel(unroll_m, UNROLL_N, true, false);
                sub(I, UNROLL_N);
                cmp(I, UNROLL_N);
                jge(".subloop31", T_NEAR);
                align(16);

                for (int i = 1; i <= 7; i++) {
                    L(l_subloop_30x[i]);
                    cmp(I, i);
                    if (i < 7) {
                        jne(l_subloop_30x[i + 1], T_NEAR);
                    } else {
                        jne(".subloop99", T_NEAR);
                    }
                    kernel(unroll_m, i, true, false);
                    if (i < 7)
                        jmp(".subloop99", T_NEAR);
                    align(16);
                }
            }
            jmp(".subloop99", T_NEAR);
            align(16);

            L(".subloop96");
            if (isTransA) {
                do_pack(unroll_m);
            }

            mov(CO1, C);
            add(C, unroll_m * SIZE);
            mov(BO1, B);
            if (!isTransB) {
                lea(BO2, ptr[B + LDB * 4]);
            }

            if (!isTransA) {
                lea(AA, ptr[A + (unroll_m + 16 - 1 - OFFSET) * SIZE]);
                cmp(M, UNROLL_M);
                jg(".subloop98mask", T_NEAR);
                mov(AA, ORIG_A);
                lea(AA, ptr[AA + (16 - 1 - OFFSET) * SIZE]);
                L(".subloop98mask");
            }

            mov(LL, N);
            mov(I, LL);
            if (!isTransA) {
                // If N is too small, skip copy operation
                cmp(LL, UNROLL_N * 3);
                jle(".subloop30mask", T_NEAR);

                // If A is not aligned to cache line
                cmp(FLAG, 0);
                je(".subloop30mask", T_NEAR);
            } else {
                cmp(LL, UNROLL_N);
                jl(l_subloop_mask_20x[1], T_NEAR);
            }
            align(16);

            if (!isTransA) {
                kernel(unroll_m, UNROLL_N, true, true, false);
            } else {
                kernel(unroll_m, UNROLL_N, false, false, false);
            }

            sub(I, UNROLL_N);
            cmp(I, UNROLL_N);
            jl(l_subloop_mask_20x[1], T_NEAR);
            align(16);

            L(".subloop11mask");
            kernel(unroll_m, UNROLL_N, false, false, false);
            sub(I, UNROLL_N);
            cmp(I, UNROLL_N);
            jge(".subloop11mask", T_NEAR);
            align(16);

            for (int i = 1; i <= 7; i++) {
                L(l_subloop_mask_20x[i]);
                cmp(I, i);
                if (i < 7) {
                    jne(l_subloop_mask_20x[i + 1], T_NEAR);
                } else {
                    jne(".subloop99", T_NEAR);
                }
                kernel(unroll_m, i, false, false, false);
                jmp(".subloop99", T_NEAR);
                align(16);
            }

            if (!isTransA) {
                L(".subloop30mask");
                cmp(I, UNROLL_N);
                jl(l_subloop_mask_30x[1], T_NEAR);
                align(16);

                L(".subloop31mask");
                kernel(unroll_m, UNROLL_N, true, false, false);
                sub(I, UNROLL_N);
                cmp(I, UNROLL_N);
                jge(".subloop31mask", T_NEAR);
                align(16);

                for (int i = 1; i <= 7; i++) {
                    L(l_subloop_mask_30x[i]);
                    cmp(I, i);
                    if (i < 7) {
                        jne(l_subloop_mask_30x[i + 1], T_NEAR);
                    } else {
                        jne(".subloop99", T_NEAR);
                    }
                    kernel(unroll_m, i, true, false, false);
                    if (i < 7)
                        jmp(".subloop99", T_NEAR);
                    align(16);
                }
            }

            L(".subloop99");
            // Compute address for A
            if (!isTransA) {
                add(A, unroll_m * SIZE);
            } else {
                mov(rax, LDA);
                imul(rax, rax, unroll_m);
                add(A, rax);
            }

            // Compute next address of BIAS
            if (hasBias) {
                add(BIAS, unroll_m * SIZE);
            }

            outLocalLabel();
        };

        inLocalLabel();

        preamble();

        // Get the registers
        mov(B, ARG_B);
        mov(LDB, ARG_LDB);
        mov(r15, ARG_BETA);
        mov(r12, ARG_C);
        if (hasBias)
            mov(r10, ARG_BIAS);
        mov(LDC, ARG_LDC);
        mov(rbp, rsp);

        vmovss(xmm0, ptr[ARG_ALPHA]);
        vmovss(xmm1, ptr[r15]);

#if _WIN32
        mov(A, ARG_A);
        mov(LDA, ARG_LDA);
#endif

        cmp(K, STACK_K_CAPACITY);
        jg(".buffer_in_ws", T_NEAR);

        // Create buffer and align to 4kB page
        lea(rax, ptr[K * SIZE]);
        imul(rax, rax, 0x30);
        add(rax, 256);
        sub(rsp, rax);
        and_(rsp, -PAGE_4K);
        jmp(".buffer_allocated", T_NEAR);

        L(".buffer_in_ws");
        mov(rsp, ARG_WS);

        L(".buffer_allocated");

        mov(ORIG_SP, rbp);
        mov(M, ARG_M);
        mov(N, ARG_N);
        mov(C, r12);
        if (hasBias)
            mov(BIAS, r10);
        vmovss(ALPHA, xmm0);
        vmovss(BETA, xmm1);
        sub(A, -OFFSET * SIZE);
        sub(B, -OFFSET * SIZE);
        mov(ORIG_A, A);
        sal(LDA, BASE_SHIFT);
        sal(LDB, BASE_SHIFT);
        sal(LDC, BASE_SHIFT);
        lea(LDB3, ptr[LDB + LDB * 2]);

        if (isTransA) {
            vpbroadcastq(zmm2, LDA);
            vpxorq(ZSTRIDE, ZSTRIDE, ZSTRIDE);
            mov(rax, -2);
            kmovw(k4, eax);

            for (int i = 0; i < 6; i++) {
                vpaddq(ZSTRIDE | k4, ZSTRIDE, zmm2);
                kshiftlw(k4, k4, 1);
            }
            vpaddq(ZSTRIDE | k4, ZSTRIDE, zmm2);
        }

        // Check A alignment and leading dimension; take copy-based path as
        // needed
        mov(rax, LDA);
        or_(rax, A);
        and_(rax, ver == ver_avx512_core ? 0x07 : 0x3f);
        mov(FLAG, rax);

        for (int i = 8; i < 16; i++) {
            for (int j = 0; j < 3; j++) {
                vpxorq(Zmm(i + 8 * j), Zmm(i + 8 * j), Zmm(i + 8 * j));
            }
        }

        cmp(M, 32);
        jle(".main0", T_NEAR);
        align(16);

        L(".main1");
        subloop(48);
        sub(M, UNROLL_M);
        cmp(M, 32);
        jg(".main1", T_NEAR);
        align(16);

        L(".main0");
        cmp(M, 16);
        jle(".main2", T_NEAR);

        subloop(32);
        jmp(".main999", T_NEAR);
        align(16);

        L(".main2");
        cmp(M, 0);
        jle(".main999", T_NEAR);
        subloop(16);
        align(16);

        L(".main999");
        // Restore original stack
        mov(rsp, ORIG_SP);

        vzeroupper();
        postamble();

        outLocalLabel();

        ker_ = reinterpret_cast<decltype(ker_)>(
                const_cast<uint8_t *>(this->getCode()));
    }

    void operator()(long long int m, long long int n, long long int k,
            const float *alpha, const float *a, long long int lda,
            const float *b, long long int ldb, const float *beta, float *c,
            long long int ldc, const float *bias, float *ws)
    {
        (*ker_)(m, n, k, alpha, a, lda, b, ldb, beta, c, ldc, bias, ws);
    }

private:
    void (*ker_)(long long int m, long long int n, long long int k,
            const float *alpha, const float *a, long long int lda,
            const float *b, long long int ldb, const float *beta, float *c,
            long long int ldc, const float *bias, float *ws);
};

typedef void (*ker)(long long int, long long int, long long int, float *,
        float *, long long int, float *, long long int, float *, float *,
        long long int, float *, float *);
void jit_avx512_common_gemm_f32::sgemm_nocopy_driver(const char *transa,
        const char *transb, int m, int n, int k, const float *alpha,
        const float *a, int lda, const float *b, int ldb, const float *beta,
        float *c, int ldc, const float *bias, float *ws)
{
    bool isTransA = (*transa == 'T' || *transa == 't');
    bool isTransB = (*transb == 'T' || *transb == 't');

    int Bm, sizeM, Bn, sizeN, Bk, sizeK;

    int i, j;

    if ((m <= 0) || (n <= 0))
        return;

    if ((k <= 0) || (alpha[0] == 0.)) {

        if (beta[0] == 0.) {
            for (j = 0; j < n; j++)
                for (i = 0; i < m; i++)
                    c[i + j * ldc] = 0.0;
        } else if (beta[0] != 1.) {
            for (j = 0; j < n; j++)
                for (i = 0; i < m; i++)
                    c[i + j * ldc] *= beta[0];
        }

        return;
    }

    int BM = 4032, BN, BK;
    if (mayiuse(avx512_core)) {
        BN = isTransA ? 384 : 64;
        BK = 384;
    } else {
        BN = isTransA ? 96 : 64;
        BK = isTransB ? 96 : 192;
        if (!isTransA && !isTransB)
            BK = 128;
    }
    const float *curA, *curB, *curBias = NULL;
    float *curC;

    for (Bk = 0; Bk < k; Bk += sizeK) {
        sizeK = k - Bk;
        if (sizeK >= BK * 2)
            sizeK = BK;
        else {
            if (sizeK > BK)
                sizeK = (sizeK + 1) / 2;
        }

        for (Bm = 0; Bm < m; Bm += sizeM) {
            sizeM = m - Bm;
            if (sizeM >= BM * 2)
                sizeM = BM;
            else {
                if (sizeM > BM + BM / 2)
                    sizeM = (sizeM + 1) / 2;
            }

            for (Bn = 0; Bn < n; Bn += sizeN) {
                sizeN = n - Bn;
                if (sizeN >= BN * 2)
                    sizeN = BN;
                else {
                    if (sizeN > BN + BN / 2)
                        sizeN = (sizeN + 1) / 2;
                }

                if (!isTransA) {
                    curA = a + Bm + Bk * lda;
                } else {
                    curA = a + Bk + Bm * lda;
                }
                if (!isTransB) {
                    curB = b + Bk + Bn * ldb;
                } else {
                    curB = b + Bn + Bk * ldb;
                }
                curC = c + Bm + Bn * ldc;
                if (bias != NULL) {
                    if (Bk == 0) {
                        curBias = bias + Bm;
                    } else {
                        curBias = NULL;
                    }
                }
                if (Bk == 0) {
                    if (*beta == 0.0 && bias == NULL)
                        (*ker_b0_)((long long int)sizeM, (long long int)sizeN,
                                (long long int)sizeK, alpha, curA,
                                (long long int)lda, curB, (long long int)ldb,
                                beta, curC, (long long int)ldc, curBias, ws);
                    else
                        (*ker_bn_)((long long int)sizeM, (long long int)sizeN,
                                (long long int)sizeK, alpha, curA,
                                (long long int)lda, curB, (long long int)ldb,
                                beta, curC, (long long int)ldc, curBias, ws);
                } else {
                    (*ker_b1_)((long long int)sizeM, (long long int)sizeN,
                            (long long int)sizeK, alpha, curA,
                            (long long int)lda, curB, (long long int)ldb, beta,
                            curC, (long long int)ldc, curBias, ws);
                }
            }
        }
    }
    return;
}

// Partition n values as equally as possible among nthr threads
// and set the offset (t_offset) and number of values (t_block) for ithr
// Assumption: 0 <= ithr < nthr
inline void jit_avx512_common_gemm_f32::partition_unit_diff(
        int ithr, int nthr, int n, int *t_offset, int *t_block)
{
    int band = n / nthr;
    if (band == 0)
        band = 1;
    int tail = n - band * nthr;
    if (tail < 0)
        tail = 0;

    if (ithr < tail) {
        band++;
        *t_offset = band * ithr;
        *t_block = band;
    } else {
        *t_offset = band * ithr + tail;
        *t_block = band;
    }

    if (*t_offset >= n) {
        *t_offset = 0;
        *t_block = 0;
    }

    if (*t_offset + *t_block > n) {
        *t_block = n - *t_offset;
    }
}

// Sum the m*n values from p_src into p_dst, assuming the two-dimensional
// arrays have leading dimensions ld_src and ld_dst, respectively
inline void jit_avx512_common_gemm_f32::sum_two_matrices(
        int m, int n, float *p_src, int ld_src, float *p_dst, int ld_dst)
{
    int i, j;
    for (j = 0; j < n; j++) {
        for (i = 0; i < m; i++) {
            p_dst[i + j * ld_dst] += p_src[i + j * ld_src];
        }
    }
}

#define BM_NOCOPY_AVX512_COMMON 32
#define BN_NOCOPY_AVX512_COMMON 64
#define BK_NOCOPY_AVX512_COMMON 192
#define BN_LARGE_NOCOPY_AVX512_COMMON 192
#define BM_SMALL_NOCOPY_AVX512_COMMON 16
#define BN_SMALL_NOCOPY_AVX512_COMMON 1
#define BK_SMALL_NOCOPY_AVX512_COMMON 4
// Determine number of threads for each dimension of a 3-D partitioning
// algorithm based on input parameters
// m/n/k - First/second/third parameter for GEMM
// nthrs - total available number of threads
// nthrs_m/nthrs_n/nthrs_k - number of threads to use in each dimension
// BM/BN/BK - blocking values
inline void jit_avx512_common_gemm_f32::calc_nthr_nocopy_avx512_common(int m,
        int n, int k, int nthrs, int *nthrs_m, int *nthrs_n, int *nthrs_k,
        int *BM, int *BN, int *BK)
{
    int nthr, nthr_m, nthr_n, nthr_k;
    int MB, NB, KB;
    nthr = nthrs;

    int counter = 0;
    float ratio_float = 1.;
    int ratio = 1;
    nthr = nthrs;
    int nthr_m_gt_n;

    /* Partition along K dimension if there is enough K and there is not enough
     * M/N */
    if (n <= 2 * BN_NOCOPY_AVX512_COMMON &&
            m <= 2 * BM_NOCOPY_AVX512_COMMON * nthr) {
        nthr_k = k / BK_NOCOPY_AVX512_COMMON;
        if (nthr_k > nthr / 4)
            nthr_k = nthr / 4;
        if (nthr_k < 1)
            nthr_k = 1;

        while ((nthr_k > 1) && (nthr % nthr_k)) {
            nthr_k--;
        }
        nthr /= nthr_k;
    } else {
        nthr_k = 1;
    }
    nthr_m = (m + BM_NOCOPY_AVX512_COMMON - 1) / BM_NOCOPY_AVX512_COMMON;
    nthr_n = (n + BN_NOCOPY_AVX512_COMMON - 1) / BN_NOCOPY_AVX512_COMMON;

    if (nthr_m < 1)
        nthr_m = 1;
    if (nthr_n < 1)
        nthr_n = 1;

    nthr_m_gt_n = nthr_m > nthr_n ? 1 : 0;
    ratio_float = (float)nthr_m / nthr_n;

    if (nthr_m_gt_n)
        ratio = (int)ratio_float;
    else
        ratio = (int)(1. / ratio_float);

    // scale down nthr_m and nthr_n if they are too large
    while (nthr_m * nthr_n > 4 * nthr) {
        nthr_m /= 2;
        nthr_n /= 2;
    }

    if (nthr_m < 1)
        nthr_m = 1;
    if (nthr_n < 1)
        nthr_n = 1;

    // Simple partition reduction
    counter = 0;
    while (nthr_m * nthr_n > nthr) {
        if (nthr_m > nthr_n) {
            if (counter < ratio)
                nthr_m--;
            else {
                nthr_n--;
                counter = -1;
            }
        } else {
            if (counter < ratio)
                nthr_n--;
            else {
                nthr_m--;
                counter = -1;
            }
        }
        counter++;
    }

    // Simple partition increment
    counter = 0;
    while (nthr_m * nthr_n < 0.95 * nthr) {
        if (nthr_m > nthr_n) {
            if (counter < ratio)
                nthr_m++;
            else {
                nthr_n++;
                counter = -1;
            }
        } else {
            if (counter < ratio)
                nthr_n++;
            else {
                nthr_m++;
                counter = -1;
            }
        }
        counter++;
    }

    // if nothing works out, then this should work
    if ((nthr_m * nthr_n > nthr)) {

        if (nthr_m <= nthr_n) {
            nthr_m = (int)sqrt((double)nthr);
            if (nthr_m > (m + BM_SMALL_NOCOPY_AVX512_COMMON - 1)
                            / BM_SMALL_NOCOPY_AVX512_COMMON)
                nthr_m = (m + BM_SMALL_NOCOPY_AVX512_COMMON - 1)
                        / BM_SMALL_NOCOPY_AVX512_COMMON;
            nthr_n = nthr / nthr_m;

            while ((nthr_m > 1) && (nthr_m * nthr_n != nthr)) {
                nthr_m--;
                nthr_n = nthr / nthr_m;
            }
        } else {
            nthr_n = (int)sqrt((double)nthr);
            if (nthr_n > (n + BN_SMALL_NOCOPY_AVX512_COMMON - 1)
                            / BN_SMALL_NOCOPY_AVX512_COMMON)
                nthr_n = (n + BN_SMALL_NOCOPY_AVX512_COMMON - 1)
                        / BN_SMALL_NOCOPY_AVX512_COMMON;
            nthr_m = nthr / nthr_n;

            while ((nthr_n > 1) && (nthr_m * nthr_n != nthr)) {
                nthr_n--;
                nthr_m = nthr / nthr_n;
            }
        }
    }

    MB = (m + nthr_m - 1) / nthr_m + BM_SMALL_NOCOPY_AVX512_COMMON - 1;
    MB -= MB % BM_SMALL_NOCOPY_AVX512_COMMON;
    NB = (n + nthr_n - 1) / nthr_n + BN_SMALL_NOCOPY_AVX512_COMMON - 1;
    NB -= NB % BN_SMALL_NOCOPY_AVX512_COMMON;
    KB = (k + nthr_k - 1) / nthr_k + BK_SMALL_NOCOPY_AVX512_COMMON - 1;
    KB -= KB % BK_SMALL_NOCOPY_AVX512_COMMON;

    if (MB * nthr_m > m)
        nthr_m = (m + MB - 1) / MB;
    if (NB * nthr_n > n)
        nthr_n = (n + NB - 1) / NB;
    if (KB * nthr_k > k)
        nthr_k = (k + KB - 1) / KB;

    *nthrs_m = nthr_m;
    *nthrs_n = nthr_n;
    *nthrs_k = nthr_k;

    *BM = MB;
    *BN = NB;
    *BK = KB;
}
#undef BM_NOCOPY_AVX512_COMMON
#undef BN_NOCOPY_AVX512_COMMON
#undef BK_NOCOPY_AVX512_COMMON
#undef BN_LARGE_NOCOPY_AVX512_COMMON
#undef BM_SMALL_NOCOPY_AVX512_COMMON
#undef BN_SMALL_NOCOPY_AVX512_COMMON
#undef BK_SMALL_NOCOPY_AVX512_COMMON

void jit_avx512_common_gemm_f32::sgemm(const char *transa, const char *transb,
        const int *p_m, const int *p_n, const int *p_k, const float *p_alpha,
        const float *A, const int *p_lda, const float *B, const int *p_ldb,
        const float *p_beta, float *C, const int *p_ldc, const float *bias)
{
    assert(*transa == transa_ && *transb == transb_ && *p_beta == beta_);
    int nthr = (omp_in_parallel()) ? 1 : omp_get_max_threads();
    int m = *p_m;
    int n = *p_n;
    int k = *p_k;
    int lda = *p_lda;
    int ldb = *p_ldb;
    int ldc = *p_ldc;
    float beta = *p_beta;
    int MB, NB, KB;

    int nthr_m, nthr_n, nthr_k, nthr_mn;

    assert(nthr <= nthrs_);

    // Determine threading partitioning
    calc_nthr_nocopy_avx512_common(
            m, n, k, nthr, &nthr_m, &nthr_n, &nthr_k, &MB, &NB, &KB);

    // May not happen, but just in case
    if (nthr < nthr_m * nthr_n * nthr_k)
        nthr = nthr_m * nthr_n * nthr_k;

    nthr_mn = nthr_m * nthr_n;

    unsigned int volatile *ompstatus = (unsigned int volatile *)ompstatus_;
    if (!ompstatus) return;

    float *c_buffers = NULL;
    float *ws_buffers = NULL;

    if (nthr_k > 1) {
        for (int i = 0; i < nthr; i++)
            ompstatus[i * CACHE_LINE_SIZE] = 0;

        c_buffers = (float *)malloc(nthr_m * nthr_n * (nthr_k - 1) * MB * NB
                * sizeof(float), PAGE_4K);
    }

    const size_t ws_elems_per_thr = k * 48 + 64;
    const size_t ws_size_per_thr
            = utils::rnd_up(ws_elems_per_thr * sizeof(float), PAGE_4K);
    if (k > STACK_K_CAPACITY) {
        ws_buffers = (float *)malloc(nthr * ws_size_per_thr, PAGE_4K);
    }

#pragma omp parallel for num_threads(nthr)
    for (int ithr_omp = 0; ithr_omp < nthr; ithr_omp++) {
        int ithr_omp_m, ithr_omp_n, ithr_omp_k, ithr_omp_mn;
        int m_from, m_to, myM;
        int n_from, n_to, myN;
        int k_from, k_to, myK;
        int cbase, ibase;
        const float *myA, *myB, *myBias = NULL;
        float *myC = C, myBeta;
        float *ws = ws_buffers ?
                ws_buffers + ithr_omp * ws_size_per_thr / sizeof(float) : 0;
        int ld = ldc;

        if (ithr_omp < nthr_m * nthr_n * nthr_k) {

            ithr_omp_mn = ithr_omp % nthr_mn;
            ithr_omp_m = ithr_omp_mn % nthr_m;
            ithr_omp_n = ithr_omp_mn / nthr_m;
            ithr_omp_k = ithr_omp / nthr_mn;

            /* swap ithr_omp_k for performance improvement */
            if (ithr_omp_k == 0)
                ithr_omp_k = nthr_k - 1;
            else if (ithr_omp_k == nthr_k - 1)
                ithr_omp_k = 0;

            m_from = MB * (ithr_omp_m);
            m_to = MB * (ithr_omp_m + 1);
            if (m_to > m)
                m_to = m;
            myM = m_to - m_from;

            n_from = NB * (ithr_omp_n);
            n_to = NB * (ithr_omp_n + 1);
            if (n_to > n)
                n_to = n;
            myN = n_to - n_from;

            k_from = KB * (ithr_omp_k);
            k_to = KB * (ithr_omp_k + 1);
            if (k_to > k)
                k_to = k;
            myK = k_to - k_from;

            cbase = (ithr_omp_m + nthr_m * ithr_omp_n) * (nthr_k - 1);
            ibase = (ithr_omp_m + nthr_m * ithr_omp_n) * nthr_k;

            if ((myM > 0) && (myN > 0)) {

                if (*transa == 'N' || *transa == 'n') {
                    myA = &(A[m_from + k_from * lda]);
                } else {
                    myA = &(A[k_from + m_from * lda]);
                }
                if (*transb == 'N' || *transb == 'n') {
                    myB = &(B[k_from + n_from * ldb]);
                } else {
                    myB = &(B[n_from + k_from * ldb]);
                }
                if (ithr_omp_k == 0) {
                    myC = &(C[m_from + n_from * ldc]);
                    myBeta = beta;
                    ld = ldc;
                    if (hasBias_)
                        myBias = &(bias[m_from]);
                } else {
                    myC = c_buffers + MB * NB * (cbase + ithr_omp_k - 1);
                    myBeta = 0.0;
                    ld = MB;
                    myBias = NULL;
                }

                sgemm_nocopy_driver(transa, transb, myM, myN, myK, p_alpha, myA,
                        lda, myB, ldb, &myBeta, myC, ld, myBias, ws);

                if (nthr_k > 1)
                    ompstatus[(ibase + ithr_omp_k) * CACHE_LINE_SIZE] = 1;
            }

            if (nthr_k > 1) {

                // sum matrices partitioned along K dimension
                int n1, n2;

                partition_unit_diff(ithr_omp_k, nthr_k, myN, &n1, &n2);

                if (ithr_omp_k > 0) {

                    myC = c_buffers + MB * NB * (cbase + ithr_omp_k - 1);
                    myC = myC + n1 * MB;
                    /* need to wait until main thread finishes */
                    while (ompstatus[ibase * CACHE_LINE_SIZE] != 1) {
                    };

                    /* my cache is hot */
                    sum_two_matrices(myM, n2, myC, MB,
                            &C[m_from + (n_from + n1) * ldc], ldc);
                }

                for (int ik = 1; ik < nthr_k; ++ik) {
                    if (ik != ithr_omp_k) {

                        myC = c_buffers + MB * NB * (cbase + ik - 1);
                        myC = myC + n1 * MB;

                        while (ompstatus[(ibase + ik) * CACHE_LINE_SIZE] != 1) {
                        };

                        sum_two_matrices(myM, n2, myC, MB,
                                &C[m_from + (n_from + n1) * ldc], ldc);
                    }
                }
            }
        }
    }

    if (nthr_k > 1)
        free(c_buffers);
    free(ws_buffers);
}

jit_avx512_common_gemm_f32::jit_avx512_common_gemm_f32(
        char transa, char transb, float beta, bool hasBias)
{
    transa_ = transa;
    transb_ = transb;
    beta_ = beta;
    hasBias_ = hasBias;
    if (hasBias) {
        assert(beta == 0.0);
    }
    ker_bn_ = new xbyak_gemm(transa, transb, beta, hasBias);
    if (beta != 1.0) {
        ker_b1_ = new xbyak_gemm(transa, transb, 1.0);
    } else {
        ker_b1_ = ker_bn_;
    }
    if (beta != 0.0 || (beta == 0.0 && hasBias)) {
        ker_b0_ = new xbyak_gemm(transa, transb, 0.0);
    } else {
        ker_b0_ = ker_bn_;
    }

    nthrs_ = omp_get_max_threads();
    ompstatus_ = (unsigned int *)malloc(
        sizeof(unsigned int *) * nthrs_ * CACHE_LINE_SIZE, 64);
    assert(ompstatus_);

}

jit_avx512_common_gemm_f32::~jit_avx512_common_gemm_f32()
{
    delete ker_bn_;
    if (beta_ != 1.0)
        delete ker_b1_;
    if (beta_ != 0.0 || (beta_ == 0.0 && hasBias_))
        delete ker_b0_;
    free(ompstatus_);
}
}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
