/*
 *  Copyright (c) 2017, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <memory>
#include <string>

#include <folly/fibers/FiberManager.h>
#include <folly/io/IOBuf.h>

#include "mcrouter/lib/McOperation.h"
#include "mcrouter/lib/McResUtil.h"
#include "mcrouter/lib/Operation.h"
#include "mcrouter/lib/OperationTraits.h"
#include "mcrouter/lib/Reply.h"
#include "mcrouter/lib/RouteHandleTraverser.h"
#include "mcrouter/lib/fbi/cpp/util.h"
#include "mcrouter/lib/mc/msg.h"
#include "mcrouter/lib/network/gen/Memcache.h"

namespace facebook {
namespace memcache {

/**
 * This route handle is intended to be used for two level caching.
 * For 'get' tries to find value in L1 cache, in case of a miss fetches value
 * from L2 cache and asynchronous 'add' request updates value in L1.
 *
 * Supports negative caching. If ncacheUpdatePeriod > 0, every miss from L2
 * will be stored in L1 as a special "ncache" value with "NEGATIVE_CACHE" flag
 * and ncacheExptime expiration time.
 * If we try to fetch "ncache" value from L1 we'll return a miss and refill
 * L1 from L2 every ncacheUpdatePeriod "ncache" requests.
 *
 * NOTE: Doesn't work with lease get, gets and metaget.
 * Always overrides expiration time for L2 -> L1 update request.
 * Client is responsible for L2 consistency, sets and deletes are forwarded
 * only to L1 cache.
 */
template <class RouteHandleIf>
class L1L2CacheRoute {
 public:
  static std::string routeName() {
    return "l1l2-cache";
  }

  template <class Request>
  void traverse(
      const Request& req,
      const RouteHandleTraverser<RouteHandleIf>& t) const {
    t(*l1_, req);
    t(*l2_, req);
  }

  L1L2CacheRoute(
      std::shared_ptr<RouteHandleIf> l1,
      std::shared_ptr<RouteHandleIf> l2,
      uint32_t upgradingL1Exptime,
      size_t ncacheExptime,
      size_t ncacheUpdatePeriod)
      : l1_(std::move(l1)),
        l2_(std::move(l2)),
        upgradingL1Exptime_(upgradingL1Exptime),
        ncacheExptime_(ncacheExptime),
        ncacheUpdatePeriod_(ncacheUpdatePeriod),
        ncacheUpdateCounter_(ncacheUpdatePeriod) {
    assert(l1_ != nullptr);
    assert(l2_ != nullptr);
  }

  template <class Request>
  ReplyT<Request> route(const Request& req, GetLikeT<Request> = 0) {
    auto l1Reply = l1_->route(req);
    if (isHitResult(l1Reply.result())) {
      if (l1Reply.flags() & MC_MSG_FLAG_NEGATIVE_CACHE) {
        if (ncacheUpdatePeriod_) {
          if (ncacheUpdateCounter_ == 1) {
            updateL1Ncache(req);
            ncacheUpdateCounter_ = ncacheUpdatePeriod_;
          } else {
            --ncacheUpdateCounter_;
          }
        }

        /* return a miss */
        l1Reply = createReply(DefaultReply, req);
      }
      return l1Reply;
    }

    /* else */
    auto l2Reply = l2_->route(req);
    if (isHitResult(l2Reply.result())) {
      folly::fibers::addTask([
        l1 = l1_,
        addReq = l1UpdateFromL2<McAddRequest>(req, l2Reply, upgradingL1Exptime_)
      ]() { l1->route(addReq); });
    } else if (isMissResult(l2Reply.result()) && ncacheUpdatePeriod_) {
      folly::fibers::addTask(
          [ l1 = l1_, addReq = l1Ncache<McAddRequest>(req, ncacheExptime_) ]() {
            l1->route(addReq);
          });
    }
    return l2Reply;
  }

  template <class Request>
  ReplyT<Request> route(const Request& req, OtherThanT<Request, GetLike<>> = 0)
      const {
    return l1_->route(req);
  }

 private:
  const std::shared_ptr<RouteHandleIf> l1_;
  const std::shared_ptr<RouteHandleIf> l2_;
  const uint32_t upgradingL1Exptime_{0};
  size_t ncacheExptime_{0};
  size_t ncacheUpdatePeriod_{0};
  size_t ncacheUpdateCounter_{0};

  template <class ToRequest, class Request, class Reply>
  static ToRequest l1UpdateFromL2(
      const Request& origReq,
      const Reply& reply,
      size_t upgradingL1Exptime) {
    ToRequest req;
    req.key() = origReq.key();
    if (auto replyValue = carbon::valuePtrUnsafe(reply)) {
      req.value() = replyValue->cloneAsValue();
    }
    req.flags() = reply.flags();
    req.exptime() = upgradingL1Exptime;
    return req;
  }

  template <class ToRequest, class Request>
  static ToRequest l1Ncache(const Request& origReq, size_t ncacheExptime) {
    ToRequest req;
    req.key() = origReq.key();
    req.value() = folly::IOBuf(folly::IOBuf::COPY_BUFFER, "ncache");
    req.flags() = MC_MSG_FLAG_NEGATIVE_CACHE;
    req.exptime() = ncacheExptime;
    return std::move(req);
  }

  template <class Request>
  void updateL1Ncache(const Request& req) {
    folly::fibers::addTask([
      l1 = l1_,
      l2 = l2_,
      creq = req,
      upgradingL1Exptime = upgradingL1Exptime_,
      ncacheExptime = ncacheExptime_
    ]() {
      auto l2Reply = l2->route(creq);
      if (isHitResult(l2Reply.result())) {
        l1->route(
            l1UpdateFromL2<McSetRequest>(creq, l2Reply, upgradingL1Exptime));
      } else {
        /* bump TTL on the ncache entry */
        l1->route(l1Ncache<McSetRequest>(creq, ncacheExptime));
      }
    });
  }
};
}
} // facebook::memcache
