/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <wangle/acceptor/ManagedConnection.h>

#include <chrono>
#include <folly/Memory.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/HHWheelTimer.h>
#include <folly/io/async/DelayedDestruction.h>
#include <folly/io/async/EventBase.h>

namespace wangle {

/**
 * A ConnectionManager keeps track of ManagedConnections.
 */
class ConnectionManager: public folly::DelayedDestruction,
                         private ManagedConnection::Callback {
 public:

  /**
   * Interface for an optional observer that's notified about
   * various events in a ConnectionManager
   */
  class Callback {
  public:
    virtual ~Callback() = default;

    /**
     * Invoked when the number of connections managed by the
     * ConnectionManager changes from nonzero to zero.
     */
    virtual void onEmpty(const ConnectionManager& cm) = 0;

    /**
     * Invoked when a connection is added to the ConnectionManager.
     */
    virtual void onConnectionAdded(const ConnectionManager& cm) = 0;

    /**
     * Invoked when a connection is removed from the ConnectionManager.
     */
    virtual void onConnectionRemoved(const ConnectionManager& cm) = 0;
  };

  typedef std::unique_ptr<ConnectionManager, Destructor> UniquePtr;

  /**
   * Returns a new instance of ConnectionManager wrapped in a unique_ptr
   */
  template<typename... Args>
  static UniquePtr makeUnique(Args&&... args) {
    return UniquePtr(new ConnectionManager(std::forward<Args>(args)...));
  }

  /**
   * Constructor not to be used by itself.
   */
  ConnectionManager(folly::EventBase* eventBase,
                    std::chrono::milliseconds timeout,
                    Callback* callback = nullptr);

  /**
   * Add a connection to the set of connections managed by this
   * ConnectionManager.
   *
   * @param connection     The connection to add.
   * @param timeout        Whether to immediately register this connection
   *                         for an idle timeout callback.
   */
  void addConnection(ManagedConnection* connection,
      bool timeout = false);

  /**
   * Schedule a timeout callback for a connection.
   */
  void scheduleTimeout(ManagedConnection* const connection,
                       std::chrono::milliseconds timeout);

  /*
   * Schedule a callback on the wheel timer
   */
  void scheduleTimeout(folly::HHWheelTimer::Callback* callback,
                       std::chrono::milliseconds timeout);

  /**
   * Remove a connection from this ConnectionManager and, if
   * applicable, cancel the pending timeout callback that the
   * ConnectionManager has scheduled for the connection.
   *
   * @note This method does NOT destroy the connection.
   */
  void removeConnection(ManagedConnection* connection);

  /* Begin gracefully shutting down connections in this ConnectionManager.
   * Notify all connections of pending shutdown, and after idleGrace,
   * begin closing idle connections.
   */
  void initiateGracefulShutdown(std::chrono::milliseconds idleGrace);

  /**
   * Destroy all connections Managed by this ConnectionManager, even
   * the ones that are busy.
   */
  void dropAllConnections();

  size_t getNumConnections() const { return conns_.size(); }

  template <typename F>
  void iterateConns(F func) {
    auto it = conns_.begin();
    while ( it != conns_.end()) {
      func(&(*it));
      it++;
    }
  }

  std::chrono::milliseconds getDefaultTimeout() const {
    return timeout_;
  }

  void setLoweredIdleTimeout(std::chrono::milliseconds timeout) {
    CHECK(timeout >= std::chrono::milliseconds(0));
    CHECK(timeout <= timeout_);
    idleConnEarlyDropThreshold_ = timeout;
  }

  /**
   * try to drop num idle connections to release system resources.  Return the
   * actual number of dropped idle connections
   */
  size_t dropIdleConnections(size_t num);

  /**
   * ManagedConnection::Callbacks
   */
  void onActivated(ManagedConnection& conn);

  void onDeactivated(ManagedConnection& conn);

 private:
  class CloseIdleConnsCallback :
      public folly::EventBase::LoopCallback,
      public folly::AsyncTimeout {
   public:
    explicit CloseIdleConnsCallback(ConnectionManager* manager)
        : folly::AsyncTimeout(manager->eventBase_),
          manager_(manager) {}

    void runLoopCallback() noexcept override {
      VLOG(3) << "Draining more conns from loop callback";
      manager_->drainAllConnections();
    }

    void timeoutExpired() noexcept override {
      VLOG(3) << "Idle grace expired";
      manager_->idleGracefulTimeoutExpired();
    }

   private:
    ConnectionManager* manager_;
  };

  enum class ShutdownState : uint8_t {
    NONE = 0,
    // All ManagedConnections receive notifyPendingShutdown
    NOTIFY_PENDING_SHUTDOWN = 1,
    // All ManagedConnections have received notifyPendingShutdown
    NOTIFY_PENDING_SHUTDOWN_COMPLETE = 2,
    // All ManagedConnections receive closeWhenIdle
    CLOSE_WHEN_IDLE = 3,
    // All ManagedConnections have received closeWhenIdle
    CLOSE_WHEN_IDLE_COMPLETE = 4,
  };

  ~ConnectionManager() = default;

  ConnectionManager(const ConnectionManager&) = delete;
  ConnectionManager& operator=(ConnectionManager&) = delete;

  /**
   * Destroy all connections managed by this ConnectionManager that
   * are currently idle, as determined by a call to each ManagedConnection's
   * isBusy() method.
   */
  void drainAllConnections();

  void idleGracefulTimeoutExpired();

  /**
   * All the managed connections. idleIterator_ seperates them into two parts:
   * idle and busy ones.  [conns_.begin(), idleIterator_) are the busy ones,
   * while [idleIterator_, conns_.end()) are the idle one. Moreover, the idle
   * ones are organized in the decreasing idle time order. */
  folly::CountedIntrusiveList<
    ManagedConnection,&ManagedConnection::listHook_> conns_;

  /** Connections that currently are registered for timeouts */
  folly::HHWheelTimer::UniquePtr connTimeouts_;

  /** Optional callback to notify of state changes */
  Callback* callback_;

  /** Event base in which we run */
  folly::EventBase* eventBase_;

  /** Iterator to the next connection to shed; used by drainAllConnections() */
  folly::CountedIntrusiveList<
    ManagedConnection,&ManagedConnection::listHook_>::iterator drainIterator_;
  folly::CountedIntrusiveList<
    ManagedConnection,&ManagedConnection::listHook_>::iterator idleIterator_;
  CloseIdleConnsCallback idleLoopCallback_;
  ShutdownState shutdownState_{ShutdownState::NONE};
  bool notifyPendingShutdown_{true};

  /**
   * the default idle timeout for downstream sessions when no system resource
   * limit is reached
   */
  std::chrono::milliseconds timeout_;

  /**
   * The idle connections can be closed earlier that their idle timeout when any
   * system resource limit is reached.  This feature can be considerred as a pre
   * load shedding stage for the system, and can be easily disabled by setting
   * idleConnEarlyDropThreshold_ to defaultIdleTimeout_. Also,
   * idleConnEarlyDropThreshold_ can be used to bottom the idle timeout. That
   * is, connection manager will not early drop the idle connections whose idle
   * time is less than idleConnEarlyDropThreshold_.
   */
  std::chrono::milliseconds idleConnEarlyDropThreshold_;
};

} // wangle
