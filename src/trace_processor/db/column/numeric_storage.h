/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef SRC_TRACE_PROCESSOR_DB_COLUMN_NUMERIC_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DB_COLUMN_NUMERIC_STORAGE_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column/utils.h"

namespace perfetto::trace_processor::column {

// Storage for all numeric type data (i.e. doubles, int32, int64, uint32).
class NumericStorageBase : public DataLayer {
 protected:
  class ChainImpl : public DataLayerChain {
   public:
    SearchValidationResult ValidateSearchConstraints(FilterOp,
                                                     SqlValue) const override;

    RangeOrBitVector SearchValidated(FilterOp, SqlValue, Range) const override;

    void IndexSearchValidated(FilterOp, SqlValue, Indices&) const override;

    void Serialize(StorageProto*) const override;

    std::string DebugString() const override { return "NumericStorage"; }

   protected:
    ChainImpl(const void* vector_ptr, ColumnType type, bool is_sorted);

   private:
    // All viable numeric values for ColumnTypes.
    using NumericValue = std::variant<uint32_t, int32_t, int64_t, double>;

    BitVector LinearSearchInternal(FilterOp op, NumericValue val, Range) const;

    Range BinarySearchIntrinsic(FilterOp op,
                                NumericValue val,
                                Range search_range) const;

    const void* vector_ptr_ = nullptr;
    const ColumnType storage_type_ = ColumnType::kDummy;
    const bool is_sorted_ = false;
  };

  NumericStorageBase(ColumnType type, bool is_sorted, Impl impl);
  ~NumericStorageBase() override;

  const ColumnType storage_type_ = ColumnType::kDummy;
  const bool is_sorted_ = false;
};

// Storage for all numeric type data (i.e. doubles, int32, int64, uint32).
template <typename T>
class NumericStorage final : public NumericStorageBase {
 public:
  PERFETTO_NO_INLINE NumericStorage(const std::vector<T>* vec,
                                    ColumnType type,
                                    bool is_sorted);

  // The implementation of this function is given by
  // make_chain.cc/make_chain_minimal.cc depending on whether this is a minimal
  // or full build of trace processor.
  std::unique_ptr<DataLayerChain> MakeChain();

 private:
  class ChainImpl : public NumericStorageBase::ChainImpl {
   public:
    ChainImpl(const std::vector<T>* vector, ColumnType type, bool is_sorted)
        : NumericStorageBase::ChainImpl(vector, type, is_sorted),
          vector_(vector) {}

    SingleSearchResult SingleSearch(FilterOp op,
                                    SqlValue sql_val,
                                    uint32_t i) const override {
      return utils::SingleSearchNumeric(op, (*vector_)[i], sql_val);
    }

    void Distinct(Indices& indices) const override {
      std::unordered_set<T> s;
      indices.tokens.erase(
          std::remove_if(indices.tokens.begin(), indices.tokens.end(),
                         [&s, this](const Token& idx) {
                           return !s.insert((*vector_)[idx.index]).second;
                         }),
          indices.tokens.end());
    }

    std::optional<Token> MaxElement(Indices& indices) const override {
      auto tok =
          std::max_element(indices.tokens.begin(), indices.tokens.end(),
                           [this](const Token& t1, const Token& t2) {
                             return (*vector_)[t1.index] < (*vector_)[t2.index];
                           });

      if (tok == indices.tokens.end()) {
        return std::nullopt;
      }

      return *tok;
    }

    std::optional<Token> MinElement(Indices& indices) const override {
      auto tok =
          std::min_element(indices.tokens.begin(), indices.tokens.end(),
                           [this](const Token& t1, const Token& t2) {
                             return (*vector_)[t1.index] < (*vector_)[t2.index];
                           });
      if (tok == indices.tokens.end()) {
        return std::nullopt;
      }

      return *tok;
    }

    SqlValue Get_AvoidUsingBecauseSlow(uint32_t index) const override {
      if constexpr (std::is_same_v<T, double>) {
        return SqlValue::Double((*vector_)[index]);
      }
      return SqlValue::Long((*vector_)[index]);
    }

    void StableSort(Token* start,
                    Token* end,
                    SortDirection direction) const override {
      const T* base = vector_->data();
      switch (direction) {
        case SortDirection::kAscending:
          std::stable_sort(start, end, [base](const Token& a, const Token& b) {
            return base[a.index] < base[b.index];
          });
          break;
        case SortDirection::kDescending:
          std::stable_sort(start, end, [base](const Token& a, const Token& b) {
            return base[a.index] > base[b.index];
          });
          break;
      }
    }

    uint32_t size() const override {
      return static_cast<uint32_t>(vector_->size());
    }

   private:
    const std::vector<T>* vector_;
  };
  Impl GetImpl() {
    if constexpr (std::is_same_v<T, double>) {
      return Impl::kNumericDouble;
    } else if constexpr (std::is_same_v<T, uint32_t>) {
      return Impl::kNumericUint32;
    } else if constexpr (std::is_same_v<T, int32_t>) {
      return Impl::kNumericInt32;
    } else if constexpr (std::is_same_v<T, int64_t>) {
      return Impl::kNumericInt64;
    } else {
      // false doesn't work as expression has to depend on the template
      // parameter
      static_assert(sizeof(T*) == 0, "T is not supported");
    }
  }

  const std::vector<T>* vector_;
};

// Define external templates to reduce binary size bloat.
extern template class NumericStorage<double>;
extern template class NumericStorage<uint32_t>;
extern template class NumericStorage<int32_t>;
extern template class NumericStorage<int64_t>;

// Define external templates to allow splitting minimal vs full targets.
extern template std::unique_ptr<DataLayerChain>
NumericStorage<double>::MakeChain();
extern template std::unique_ptr<DataLayerChain>
NumericStorage<uint32_t>::MakeChain();
extern template std::unique_ptr<DataLayerChain>
NumericStorage<int32_t>::MakeChain();
extern template std::unique_ptr<DataLayerChain>
NumericStorage<int64_t>::MakeChain();

}  // namespace perfetto::trace_processor::column

#endif  // SRC_TRACE_PROCESSOR_DB_COLUMN_NUMERIC_STORAGE_H_
