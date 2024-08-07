// Protocol Buffers - Google's data interchange format
// Copyright 2024 Google Inc.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#import <Foundation/Foundation.h>

#import "GPBUnknownFields.h"

@class GPBCodedOutputStream;

@interface GPBUnknownFields ()

- (size_t)serializedSize;
- (nonnull NSData *)serializeAsData;
- (void)writeToCodedOutputStream:(nonnull GPBCodedOutputStream *)output;

@end
