// Copyright 2015-2016 Alibaba Inc. All Rights Reserved.
// Author:
//     LuoFan luofan.zp@alibaba-inc.com
// Normalizer:
//     LuoFan luofan.zp@alibaba-inc.com


#ifndef OB_SQL_UDR_OB_UDR_CALLBACK_H_
#define OB_SQL_UDR_OB_UDR_CALLBACK_H_

#include "sql/udr/ob_udr_item_mgr.h"

namespace oceanbase
{
namespace sql
{

class ObUDRAtomicOp
{
protected:
  typedef common::hash::HashMapPair<ObUDRItemMgr::UDRKey, ObUDRItemMgr::UDRKeyNodePair*> RuleItemKV;

public:
  ObUDRAtomicOp()
    : rule_node_(NULL)
  {
  }
  virtual ~ObUDRAtomicOp() {}
  virtual int get_value(ObUDRItemMgr::UDRKeyNodePair *&rule_node);
  // get rule node and increase reference count
  void operator()(RuleItemKV &entry);

protected:
  // when get value, need lock
  virtual int lock(ObUDRItemMgr::UDRKeyNodePair &rule_node) = 0;
protected:
  ObUDRItemMgr::UDRKeyNodePair *rule_node_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObUDRAtomicOp);
};

class ObUDRWlockAndRefGuard : public ObUDRAtomicOp
{
public:
  ObUDRWlockAndRefGuard() : ObUDRAtomicOp()
  {
  }
  virtual ~ObUDRWlockAndRefGuard();
  int lock(ObUDRItemMgr::UDRKeyNodePair &rule_node)
  {
    return rule_node.lock(false/*wlock*/);
  };
private:
  DISALLOW_COPY_AND_ASSIGN(ObUDRWlockAndRefGuard);
};

class ObUDRRlockAndRefGuard : public ObUDRAtomicOp
{
public:
  ObUDRRlockAndRefGuard() : ObUDRAtomicOp()
  {
  }
  virtual ~ObUDRRlockAndRefGuard();
  int lock(ObUDRItemMgr::UDRKeyNodePair &rule_node)
  {
    return rule_node.lock(true/*rlock*/);
  };
private:
  DISALLOW_COPY_AND_ASSIGN(ObUDRRlockAndRefGuard);
};

}
}
#endif
