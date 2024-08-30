//  Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
//  Copyright (c) 2017 International Business Machines Corp.
//  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned crc32c_ppc(unsigned crc, const void *buffer, size_t len);

#ifdef __cplusplus
}
#endif
