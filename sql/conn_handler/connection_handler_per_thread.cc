/*
   Copyright (c) 2013, 2021, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "connection_handler_impl.h"

#include "channel_info.h"                // Channel_info
#include "connection_handler_manager.h"  // Connection_handler_manager
#include "mysqld.h"                      // max_connections
#include "mysqld_error.h"                // ER_*
#include "mysqld_thd_manager.h"          // Global_THD_manager
#include "sql_audit.h"                   // mysql_audit_release
#include "sql_class.h"                   // THD
#include "sql_connect.h"                 // close_connection
#include "sql_parse.h"                   // do_command
#include "sql_thd_internal_api.h"        // thd_set_thread_stack
#include "log.h"                         // Error_log_throttle


// Initialize static members
ulong Per_thread_connection_handler::blocked_pthread_count= 0;
ulong Per_thread_connection_handler::slow_launch_threads = 0;
ulong Per_thread_connection_handler::max_blocked_pthreads= 0;
std::list<Channel_info*> *Per_thread_connection_handler
                            ::waiting_channel_info_list= NULL;
mysql_mutex_t Per_thread_connection_handler::LOCK_thread_cache;
mysql_cond_t Per_thread_connection_handler::COND_thread_cache;
mysql_cond_t Per_thread_connection_handler::COND_flush_thread_cache;

// Error log throttle for the thread creation failure in add_connection method.
static
Error_log_throttle create_thd_err_log_throttle(Log_throttle
                                               ::LOG_THROTTLE_WINDOW_SIZE,
                                               sql_print_error,
                                               "Error log throttle: %10lu"
                                               " 'Can't create thread to"
                                               " handle new connection'"
                                               " error(s) suppressed");

/*
  Number of pthreads currently being woken up to handle new connections.
  Protected by LOCK_thread_cache.
*/
static uint wake_pthread= 0;
/*
  Set if we are trying to kill of pthreads in the thread cache.
  Protected by LOCK_thread_cache.
*/
static uint kill_blocked_pthreads_flag= 0;


#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_thread_cache;

static PSI_mutex_info all_per_thread_mutexes[]=
{
  { &key_LOCK_thread_cache, "LOCK_thread_cache", PSI_FLAG_GLOBAL}
};

static PSI_cond_key key_COND_thread_cache;
static PSI_cond_key key_COND_flush_thread_cache;

static PSI_cond_info all_per_thread_conds[]=
{
  { &key_COND_thread_cache, "COND_thread_cache", PSI_FLAG_GLOBAL},
  { &key_COND_flush_thread_cache, "COND_flush_thread_cache", PSI_FLAG_GLOBAL}
};
#endif


void Per_thread_connection_handler::init()
{
#ifdef HAVE_PSI_INTERFACE
  int count= array_elements(all_per_thread_mutexes);
  mysql_mutex_register("sql", all_per_thread_mutexes, count);

  count= array_elements(all_per_thread_conds);
  mysql_cond_register("sql", all_per_thread_conds, count);
#endif

  mysql_mutex_init(key_LOCK_thread_cache, &LOCK_thread_cache,
                   MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_COND_thread_cache, &COND_thread_cache);
  mysql_cond_init(key_COND_flush_thread_cache, &COND_flush_thread_cache);
  waiting_channel_info_list= new (std::nothrow) std::list<Channel_info*>;
  assert(waiting_channel_info_list != NULL);
}


void Per_thread_connection_handler::destroy()
{
  if (waiting_channel_info_list != NULL)
  {
    delete waiting_channel_info_list;
    waiting_channel_info_list= NULL;
    mysql_mutex_destroy(&LOCK_thread_cache);
    mysql_cond_destroy(&COND_thread_cache);
    mysql_cond_destroy(&COND_flush_thread_cache);
  }
}


/**
  Block the current pthread for reuse by new connections.

  @retval NULL   Too many pthreads blocked already or shutdown in progress.
  @retval !NULL  Pointer to Channel_info object representing the new connection
                 to be served by this pthread.
*/

Channel_info* Per_thread_connection_handler::block_until_new_connection()
{
  Channel_info *new_conn= NULL;
  mysql_mutex_lock(&LOCK_thread_cache);
  if (blocked_pthread_count < max_blocked_pthreads &&
      !kill_blocked_pthreads_flag)
  {
    /* Don't kill the pthread, just block it for reuse */
    DBUG_PRINT("info", ("Blocking pthread for reuse"));

    /*
      mysys_var is bound to the physical thread,
      so make sure mysys_var->dbug is reset to a clean state
      before picking another session in the thread cache.
    */
    DBUG_POP();
    assert( ! _db_is_pushed_());

    // Block pthread
    // 阻塞系统线程(pthread)
    blocked_pthread_count++;
    // 三种可BREAK阻塞，abort程序中止、等待线程数、KILL线程标志
    while (!abort_loop && !wake_pthread && !kill_blocked_pthreads_flag)
      mysql_cond_wait(&COND_thread_cache, &LOCK_thread_cache);
    blocked_pthread_count--;

    if (kill_blocked_pthreads_flag)
      mysql_cond_signal(&COND_flush_thread_cache);
    else if (wake_pthread)
    {
      // 等待线程数-1
      wake_pthread--;
      // 从等待连接的池子中POP一个连接，然后开始挂靠到当前线程上
      if (!waiting_channel_info_list->empty())
      {
        // 所以，有闲置线程可用时，连接都是从这来的，并非全部连接都要走创建线程那一步
        new_conn = waiting_channel_info_list->front();
        waiting_channel_info_list->pop_front();
        DBUG_PRINT("info", ("waiting_channel_info_list->pop %p", new_conn));
      }
      else
      {
        assert(0);
      }
    }
  }
  mysql_mutex_unlock(&LOCK_thread_cache);
  return new_conn;
}


/**
  Construct and initialize a THD object for a new connection.
  为新连接创建并初始化一个THD对象，返回THD对象都指针

  @param channel_info  Channel_info object representing the new connection.
                       Will be destroyed by this function.

  @retval NULL   Initialization failed.
  @retval !NULL  Pointer to new THD object for the new connection.
*/

static THD* init_new_thd(Channel_info *channel_info)
{
  // 根据Channel_info创建THD
  // 如果是TCP方式连接，Channel_info_tcpip_socket的create_thd方法
  THD *thd= channel_info->create_thd();
  if (thd == NULL)
  {
    channel_info->send_error_and_close_channel(ER_OUT_OF_RESOURCES, 0, false);
    delete channel_info;
    return NULL;
  }

  // 通过全局线程管理器分配一个新的线程ID
  thd->set_new_thread_id();

  // 线程创建时间、线程启动时间
  thd->start_utime= thd->thr_create_utime= my_micro_time();
  if (channel_info->get_prior_thr_create_utime() != 0)
  {
    /*
      A pthread was created to handle this connection:
      increment slow_launch_threads counter if it took more than
      slow_launch_time seconds to create the pthread.
    */
    ulong launch_time= (ulong) (thd->thr_create_utime -
                                channel_info->get_prior_thr_create_utime());
    if (launch_time >= slow_launch_time * 1000000L)
      Per_thread_connection_handler::slow_launch_threads++;
  }
  delete channel_info;

  /*
    handle_one_connection() is normally the only way a thread would
    start and would always be on the very high end of the stack ,
    therefore, the thread stack always starts at the address of the
    first local variable of handle_one_connection, which is thd. We
    need to know the start of the stack so that we could check for
    stack overruns.
  */
  thd_set_thread_stack(thd, (char*) &thd);
  if (thd->store_globals())
  {
    close_connection(thd, ER_OUT_OF_RESOURCES);
    thd->release_resources();
    delete thd;
    return NULL;
  }

  return thd;
}


/**
  Thread handler for a connection
  [重要]MySQL连接的线程处理器

  @param arg   Connection object (Channel_info)

  This function (normally) does the following:
  - Initialize thread
  - Initialize THD to be used with this thread
  - Authenticate user
  - Execute all queries sent on the connection
  - Take connection down
  - End thread  / Handle next connection using thread from thread cache
*/

extern "C" void *handle_connection(void *arg)
{
  // 全局THD管理器
  Global_THD_manager *thd_manager= Global_THD_manager::get_instance();
  Connection_handler_manager *handler_manager=
    Connection_handler_manager::get_instance();
  Channel_info* channel_info= static_cast<Channel_info*>(arg);
  bool pthread_reused MY_ATTRIBUTE((unused))= false;

  if (my_thread_init())
  {
    connection_errors_internal++;
    channel_info->send_error_and_close_channel(ER_OUT_OF_RESOURCES, 0, false);
    handler_manager->inc_aborted_connects();
    Connection_handler_manager::dec_connection_count();
    delete channel_info;
    my_thread_exit(0);
    return NULL;
  }

  /* 第一层死循环，为了连接池线程的重用*/
  for (;;)
  {
    // 根据Channel信息创建一个THD对象
    THD *thd= init_new_thd(channel_info);
    if (thd == NULL)
    {
      connection_errors_internal++;
      handler_manager->inc_aborted_connects();
      Connection_handler_manager::dec_connection_count();
      break; // We are out of resources, no sense in continuing.
    }

    // 重用连接线程
#ifdef HAVE_PSI_THREAD_INTERFACE
    if (pthread_reused)
    {
      /*
        Reusing existing pthread:
        Create new instrumentation for the new THD job,
        and attach it to this running pthread.
      */
      PSI_thread *psi= PSI_THREAD_CALL(new_thread)
        (key_thread_one_connection, thd, thd->thread_id());
      PSI_THREAD_CALL(set_thread_os_id)(psi);
      PSI_THREAD_CALL(set_thread)(psi);
    }
#endif

#ifdef HAVE_PSI_THREAD_INTERFACE
    /* Find the instrumented thread */
    PSI_thread *psi= PSI_THREAD_CALL(get_thread)();
    /* Save it within THD, so it can be inspected */
    thd->set_psi(psi);
#endif /* HAVE_PSI_THREAD_INTERFACE */
    // 为当前系统线程（pthread）分配THD对象
    mysql_thread_set_psi_id(thd->thread_id());
    mysql_thread_set_psi_THD(thd);
    mysql_socket_set_thread_owner(
      thd->get_protocol_classic()->get_vio()->mysql_socket);

    // 将THD添加到全局THD管理器中
    thd_manager->add_thd(thd);

    // 准备连接，以达到处理客户端请求的状态，包含登录验证
    if (thd_prepare_connection(thd))
      handler_manager->inc_aborted_connects();
    else
    {
      // 死循环开始: 检查客户端连接是否存活可用
      while (thd_connection_alive(thd))
      {
        // BREAKPOINT
        // [核心]开始处理客户端发起的命令
        // 每处理完一条命令，就会循环一次
        if (do_command(thd))
          break;
      }
      end_connection(thd);
    }
    // 连接失活后，就关闭连接
    close_connection(thd, 0, false, false);

    // 释放资源
    thd->get_stmt_da()->reset_diagnostics_area();
    thd->release_resources();

    // Clean up errors now, before possibly waiting for a new connection.
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_remove_thread_state(0);
#endif /* OPENSSL_VERSION_NUMBER < 0x10100000L */

    // 将THD从管理器中移除
    thd_manager->remove_thd(thd);
    Connection_handler_manager::dec_connection_count();

#ifdef HAVE_PSI_THREAD_INTERFACE
    /*
      Delete the instrumentation for the job that just completed.
    */
    thd->set_psi(NULL);
    PSI_THREAD_CALL(delete_current_thread)();
#endif /* HAVE_PSI_THREAD_INTERFACE */

    // THD对象也删除
    delete thd;

    // 如果全局退出SERVER，那么循环也结束，也就意味着该系统线程也结束
    if (abort_loop) // Server is shutting down so end the pthread.
      break;

    // BREAKPOINT
    // 回收线程，等待下个连接复用
    // 然后当前系统线程就一直阻塞在这里，直到有新连接进来或者abort
    channel_info= Per_thread_connection_handler::block_until_new_connection();
    if (channel_info == NULL)
      break;
    pthread_reused= true;
    if (abort_loop)
    {
      // Close the channel and exit as server is undergoing shutdown.
      channel_info->send_error_and_close_channel(ER_SERVER_SHUTDOWN, 0, false);
      delete channel_info;
      channel_info = NULL;
      Connection_handler_manager::dec_connection_count();
      break;
    }
  }

  // 系统线程结束的收尾工作
  my_thread_end();
  my_thread_exit(0);
  return NULL;
}


void Per_thread_connection_handler::kill_blocked_pthreads()
{
  mysql_mutex_lock(&LOCK_thread_cache);
  kill_blocked_pthreads_flag++;
  while (Per_thread_connection_handler::blocked_pthread_count)
  {
    mysql_cond_broadcast(&COND_thread_cache);
    mysql_cond_wait(&COND_flush_thread_cache, &LOCK_thread_cache);
  }
  kill_blocked_pthreads_flag--;
  mysql_mutex_unlock(&LOCK_thread_cache);
}


bool Per_thread_connection_handler::check_idle_thread_and_enqueue_connection(
                                                  Channel_info* channel_info)
{
  bool res= true;

  mysql_mutex_lock(&LOCK_thread_cache);
  // 如果阻塞的线程数大于等待线程数，意味着有闲置的线程可用
  if (Per_thread_connection_handler::blocked_pthread_count > wake_pthread)
  {
    DBUG_PRINT("info",("waiting_channel_info_list->push %p", channel_info));
    // 将该连接放入等待队列中，直到有闲置线程时再取出来建立连接
    waiting_channel_info_list->push_back(channel_info);
    // 等待线程数+1
    wake_pthread++;
    mysql_cond_signal(&COND_thread_cache);
    // 返回FALSE，表示不创建新线程
    res= false;
  }
  mysql_mutex_unlock(&LOCK_thread_cache);

  return res;
}


bool Per_thread_connection_handler::add_connection(Channel_info* channel_info)
{
  int error= 0;
  my_thread_handle id;

  DBUG_ENTER("Per_thread_connection_handler::add_connection");

  // Simulate thread creation for test case before we check thread cache
  DBUG_EXECUTE_IF("fail_thread_create", error= 1; goto handle_error;);

  // 检查是否有闲置的线程可供使用，如果有闲置线程则添加到等待队列中，并直接返回、不再创建线程
  if (!check_idle_thread_and_enqueue_connection(channel_info))
    DBUG_RETURN(false);

  /*
    There are no idle threads avaliable to take up the new
    connection. Create a new thread to handle the connection
  */
  // 如果没有闲置线程可用，那么就创建个线程来处理该连接
  // 创建系统线程，并交由handle_connection()方法去处理新线程
  // channel_info作为参数传递给新线程的handle_connection()
  channel_info->set_prior_thr_create_utime();
  error= mysql_thread_create(key_thread_one_connection, &id,
                             &connection_attrib,
                             handle_connection,
                             (void*) channel_info);
#ifndef NDEBUG
handle_error:
#endif // !NDEBUG

  if (error)
  {
    connection_errors_internal++;
    if (!create_thd_err_log_throttle.log())
      sql_print_error("Can't create thread to handle new connection(errno= %d)",
                      error);
    channel_info->send_error_and_close_channel(ER_CANT_CREATE_THREAD,
                                               error, true);
    Connection_handler_manager::dec_connection_count();
    DBUG_RETURN(true);
  }

  // 已创建线程数自增+1
  Global_THD_manager::get_instance()->inc_thread_created();
  DBUG_PRINT("info",("Thread created"));
  DBUG_RETURN(false);
}


uint Per_thread_connection_handler::get_max_threads() const
{
  return max_connections;
}
