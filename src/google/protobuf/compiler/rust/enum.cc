// Protocol Buffers - Google's data interchange format
// Copyright 2023 Google LLC.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "google/protobuf/compiler/rust/enum.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/absl_check.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "google/protobuf/compiler/cpp/helpers.h"
#include "google/protobuf/compiler/rust/context.h"
#include "google/protobuf/descriptor.h"

namespace google {
namespace protobuf {
namespace compiler {
namespace rust {

namespace {

// Converts an UpperCamel or lowerCamel string to a snake_case string.
std::string camelToSnakeCase(absl::string_view input) {
  std::string result;
  result.reserve(input.size() + 4);  // No reallocation for 4 _
  bool is_first_character = true;
  for (const char c : input) {
    if (c == '_') {
      result += '_';
      continue;
    }
    if (!is_first_character && absl::ascii_isupper(c)) {
      result += '_';
    }
    result += absl::ascii_tolower(c);
    is_first_character = false;
  }
  return result;
}

// Converts a SCREAMING_SNAKE_CASE string to an UpperCamelCase string.
std::string screamingSnakeToUpperCamelCase(absl::string_view input) {
  std::string result;
  bool cap_next_letter = true;
  for (const char c : input) {
    if (absl::ascii_isalpha(c)) {
      if (cap_next_letter) {
        result += absl::ascii_toupper(c);
      } else {
        result += absl::ascii_tolower(c);
      }
      cap_next_letter = false;
    } else if (absl::ascii_isdigit(c)) {
      result += c;
      cap_next_letter = true;
    } else {
      cap_next_letter = true;
    }
  }
  return result;
}

std::string enumName(const EnumDescriptor& desc) {
  return cpp::UnderscoresToCamelCase(desc.name(), /*cap first letter=*/true);
}

// An enum value with a unique number and any aliases for it.
struct RustEnumValue {
  // The canonical CamelCase name in Rust.
  std::string name;
  int32_t number;
  std::vector<std::string> aliases;
};

// Returns the list of rust enum variants to produce, along with their aliases.
// Each `.number` in the output is unique.
std::vector<RustEnumValue> enumValues(const EnumDescriptor& desc) {
  // Enum values may have a prefix of the name of the enum stripped from the
  // value names in the gencode. This prefix is flexible:
  // - It can be the original enum name, the name as UpperCamel, or snake_case.
  // - The stripped prefix may also end in an underscore.

  // The set of prefixes that will be stripped.
  std::initializer_list<std::string> prefixes = {
      desc.name(),
      screamingSnakeToUpperCamelCase(desc.name()),
      camelToSnakeCase(desc.name()),
  };

  absl::flat_hash_set<std::string> seen_by_name;
  absl::flat_hash_map<int32_t, RustEnumValue*> seen_by_number;
  std::vector<RustEnumValue> result;
  // The below code depends on pointer stability of elements in `result`;
  // this reserve must not be too low.
  result.reserve(static_cast<size_t>(desc.value_count()));
  seen_by_name.reserve(static_cast<size_t>(desc.value_count()));
  seen_by_number.reserve(static_cast<size_t>(desc.value_count()));

  for (int i = 0; i < desc.value_count(); ++i) {
    const EnumValueDescriptor& value = *desc.value(i);
    absl::string_view base_value_name = value.name();

    for (absl::string_view prefix : prefixes) {
      if (absl::StartsWithIgnoreCase(base_value_name, prefix)) {
        base_value_name.remove_prefix(prefix.size());

        // Also strip a joining underscore, if present.
        absl::ConsumePrefix(&base_value_name, "_");

        // Only strip one prefix.
        break;
      }
    }

    if (base_value_name.empty()) {
      // The enum value name has a similar name to the enum - don't strip.
      base_value_name = value.name();
    }

    int32_t number = value.number();
    std::string rust_value_name =
        screamingSnakeToUpperCamelCase(base_value_name);

    // Invalid identifiers are prefixed with `_`.
    if (absl::ascii_isdigit(rust_value_name[0])) {
      rust_value_name = absl::StrCat("_", rust_value_name);
    }

    if (seen_by_name.contains(rust_value_name)) {
      // Don't add an alias with the same normalized name.
      continue;
    }

    if (auto it = seen_by_number.find(number); it != seen_by_number.end()) {
      // This number has been seen before; this name is an alias.
      it->second->aliases.push_back(rust_value_name);
    } else {
      // This is the first value with this number; this name is canonical.
      result.push_back(RustEnumValue{
          .name = rust_value_name,
          .number = number,
      });
      seen_by_number.insert({number, &result.back()});
    }

    seen_by_name.insert(std::move(rust_value_name));
  }
  return result;
}

}  // namespace

void GenerateEnumDefinition(Context<EnumDescriptor> enum_) {
  const EnumDescriptor& desc = enum_.desc();
  std::string name = enumName(desc);
  ABSL_CHECK(desc.value_count() > 0);
  std::vector<RustEnumValue> values = enumValues(desc);

  enum_.Emit(
      {
          {"name", name},
          {"variants",
           [&] {
             for (const auto& value : values) {
               std::string number_str = absl::StrCat(value.number);
               // TODO: Replace with open enum variants when stable
               enum_.Emit(
                   {{"variant_name", value.name}, {"number", number_str}},
                   R"rs(
                    pub const $variant_name$: $name$ = $name$($number$);
                    )rs");
               for (const auto& alias : value.aliases) {
                 enum_.Emit({{"alias_name", alias}, {"number", number_str}},
                            R"rs(
                            pub const $alias_name$: $name$ = $name$($number$);
                            )rs");
               }
             }
           }},
          // The default value of an enum is the first listed value.
          // The compiler checks that this is equal to 0 for open enums.
          {"default_int_value", absl::StrCat(desc.value(0)->number())},
          {"impl_from_i32",
           [&] {
             if (desc.is_closed()) {
               enum_.Emit({{"name", name},
                           {"known_values_pattern",
                            [&] {
                              for (size_t i = 0; i < values.size(); ++i) {
                                // TODO: Check validity in UPB/C++.
                                enum_.Emit(absl::StrCat(
                                    values[i].number,
                                    i < values.size() - 1 ? "|" : ""));
                              }
                            }}},
                          R"rs(
              impl $std$::convert::TryFrom<i32> for $name$ {
                type Error = $pb$::UnknownEnumValue<Self>;

                fn try_from(val: i32) -> Result<$name$, Self::Error> {
                  if matches!(val, $known_values_pattern$) {
                    Ok(Self(val))
                  } else {
                    Err($pb$::UnknownEnumValue::new($pbi$::Private, val))
                  }
                }
              }
            )rs");
             } else {
               enum_.Emit({{"name", name}}, R"rs(
              impl $std$::convert::From<i32> for $name$ {
                fn from(val: i32) -> $name$ {
                  Self(val)
                }
              }
            )rs");
             }
           }},
      },
      R"rs(
      #[repr(transparent)]
      #[derive(Clone, Copy, PartialEq, Eq)]
      pub struct $name$(i32);

      #[allow(non_upper_case_globals)]
      impl $name$ {
        $variants$
      }

      impl $std$::convert::From<$name$> for i32 {
        fn from(val: $name$) -> i32 {
          val.0
        }
      }

      $impl_from_i32$

      impl $std$::default::Default for $name$ {
        fn default() -> Self {
          Self($default_int_value$)
        }
      }

      impl $std$::fmt::Debug for $name$ {
        fn fmt(&self, f: &mut $std$::fmt::Formatter<'_>) -> $std$::fmt::Result {
          f.debug_tuple(stringify!($name$)).field(&self.0).finish()
        }
      }

      impl $pb$::Proxied for $name$ {
        type View<'a> = $name$;
        type Mut<'a> = $pb$::PrimitiveMut<'a, $name$>;
      }

      impl $pb$::ViewProxy<'_> for $name$ {
        type Proxied = $name$;

        fn as_view(&self) -> $name$ {
          *self
        }

        fn into_view<'shorter>(self) -> $pb$::View<'shorter, $name$> {
          self
        }
      }

      impl $pb$::SettableValue<$name$> for $name$ {
        fn set_on<'msg>(
            self,
            private: $pbi$::Private,
            mut mutator: $pb$::Mut<'msg, $name$>
        ) where $name$: 'msg {
          mutator.set_primitive(private, self)
        }
      }

      impl $pb$::ProxiedWithPresence for $name$ {
        type PresentMutData<'a> = $pbi$::RawVTableOptionalMutatorData<'a, $name$>;
        type AbsentMutData<'a> = $pbi$::RawVTableOptionalMutatorData<'a, $name$>;

        fn clear_present_field(
          present_mutator: Self::PresentMutData<'_>,
        ) -> Self::AbsentMutData<'_> {
          present_mutator.clear($pbi$::Private)
        }

        fn set_absent_to_default(
          absent_mutator: Self::AbsentMutData<'_>,
        ) -> Self::PresentMutData<'_> {
          absent_mutator.set_absent_to_default($pbi$::Private)
        }
      }

      unsafe impl $pb$::ProxiedInRepeated for $name$ {
        fn repeated_len(r: $pb$::View<$pb$::Repeated<Self>>) -> usize {
          $pbr$::cast_enum_repeated_view($pbi$::Private, r).len()
        }

        fn repeated_push(r: $pb$::Mut<$pb$::Repeated<Self>>, val: $name$) {
          $pbr$::cast_enum_repeated_mut($pbi$::Private, r).push(val.into())
        }

        fn repeated_clear(r: $pb$::Mut<$pb$::Repeated<Self>>) {
          $pbr$::cast_enum_repeated_mut($pbi$::Private, r).clear()
        }

        unsafe fn repeated_get_unchecked(
            r: $pb$::View<$pb$::Repeated<Self>>,
            index: usize,
        ) -> $pb$::View<$name$> {
          // SAFETY: In-bounds as promised by the caller.
          unsafe {
            $pbr$::cast_enum_repeated_view($pbi$::Private, r)
              .get_unchecked(index)
              .try_into()
              .unwrap_unchecked()
          }
        }

        unsafe fn repeated_set_unchecked(
            r: $pb$::Mut<$pb$::Repeated<Self>>,
            index: usize,
            val: $name$,
        ) {
          // SAFETY: In-bounds as promised by the caller.
          unsafe {
            $pbr$::cast_enum_repeated_mut($pbi$::Private, r)
              .set_unchecked(index, val.into())
          }
        }

        fn repeated_copy_from(
            src: $pb$::View<$pb$::Repeated<Self>>,
            dest: $pb$::Mut<$pb$::Repeated<Self>>,
        ) {
          $pbr$::cast_enum_repeated_mut($pbi$::Private, dest)
            .copy_from($pbr$::cast_enum_repeated_view($pbi$::Private, src))
        }
      }

      impl $pbi$::PrimitiveWithRawVTable for $name$ {}

      // SAFETY: this is an enum type
      unsafe impl $pbi$::Enum for $name$ {
        const NAME: &'static str = "$name$";
      }
      )rs");
}

}  // namespace rust
}  // namespace compiler
}  // namespace protobuf
}  // namespace google
