// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   xiaotao.ht <xiaotao.ht@oceanbase.com>

#pragma once

#include "common/object/ob_object.h"
#include "share/ob_autoincrement_param.h"
#include "share/ob_autoincrement_service.h"
#include "sql/engine/expr/ob_expr_autoinc_nextval.h"
#include "storage/blocksstable/ob_datum_row.h"

namespace oceanbase
{
namespace observer
{
class ObTableLoadAutoincNextval
{
public:
  static int eval_nextval(share::AutoincParam *autoinc_param, blocksstable::ObStorageDatum &datum,
                          const common::ObObjTypeClass &tc, const uint64_t &sql_mode);

private:
  static int get_uint_value(blocksstable::ObStorageDatum &datum, bool &is_zero,
                            uint64_t &casted_value, const common::ObObjTypeClass &tc);
  static int get_input_value(blocksstable::ObStorageDatum &datum,
                             share::AutoincParam &autoinc_param, bool &is_to_generate,
                             uint64_t &casted_value, const common::ObObjTypeClass &tc,
                             const uint64_t &sql_mode);
  static int generate_autoinc_value(share::ObAutoincrementService &auto_service,
                                    share::AutoincParam *autoinc_param, uint64_t &new_val);
};
} // namespace observer
} // namespace oceanbase