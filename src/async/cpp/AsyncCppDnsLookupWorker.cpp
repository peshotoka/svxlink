/**
@file	 AsyncCppDnsLookupWorker.cpp
@brief   Contains a class to execute DNS queries in the Posix environment
@author  Tobias Blomberg
@date	 2003-04-17

This file contains a class for executing DNS quries in the Cpp variant of
the async environment. This class should never be used directly. It is
used by Async::CppApplication to execute DNS queries.

\verbatim
Async - A library for programming event driven applications
Copyright (C) 2003-2022 Tobias Blomberg

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/

/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <unistd.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>

#include <cassert>
#include <cstring>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncDnsLookup.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "AsyncCppDnsLookupWorker.h"



/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace Async;



/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Prototypes
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/




/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Public member functions
 *
 ****************************************************************************/


CppDnsLookupWorker::CppDnsLookupWorker(const DnsLookup& dns)
  : DnsLookupWorker(dns)
{
  m_notifier_watch.activity.connect(
      sigc::mem_fun(*this, &CppDnsLookupWorker::notificationReceived));
} /* CppDnsLookupWorker::CppDnsLookupWorker */


CppDnsLookupWorker::~CppDnsLookupWorker(void)
{
  abortLookup();
} /* CppDnsLookupWorker::~CppDnsLookupWorker */


DnsLookupWorker& CppDnsLookupWorker::operator=(DnsLookupWorker&& other_base)
{
  this->DnsLookupWorker::operator=(std::move(other_base));
  auto& other = static_cast<CppDnsLookupWorker&>(other_base);

  abortLookup();

  m_result = std::move(other.m_result);
  assert(!other.m_result.valid());

  m_notifier_watch = std::move(other.m_notifier_watch);

  return *this;
} /* CppDnsLookupWorker::operator=(DnsLookupWorker&&) */


/****************************************************************************
 *
 * Protected member functions
 *
 ****************************************************************************/

bool CppDnsLookupWorker::doLookup(void)
{
    // A lookup is already running
  if (m_result.valid())
  {
    return true;
  }

  setLookupFailed(false);

  struct in_addr inp;
  if (inet_aton(dns().label().c_str(), &inp) == 1)
  {
    addResourceRecord(
        new DnsResourceRecordA(dns().label(), 1, IpAddress(inp)));
    workerDone();
    return true;
  }

  int fd[2];
  if (pipe(fd) != 0)
  {
    char errbuf[256];
    strerror_r(errno, errbuf, sizeof(errbuf));
    std::cerr << "*** ERROR: Could not create pipe: " << errbuf << std::endl;
    setLookupFailed();
    return false;
  }
  m_notifier_watch.setFd(fd[0], FdWatch::FD_WATCH_RD);
  m_notifier_watch.setEnabled(true);

  ThreadContext ctx;
  ctx.label = dns().label();
  ctx.type = dns().type();
  ctx.notifier_wr = fd[1];
  ctx.anslen = 0;
  ctx.thread_cerr.clear();
  m_result = std::move(std::async(std::launch::async, workerFunc,
        std::move(ctx)));

  return true;

} /* CppDnsLookupWorker::doLookup */


void CppDnsLookupWorker::abortLookup(void)
{
  m_result = std::move(std::future<ThreadContext>());

  int fd = m_notifier_watch.fd();
  if (fd >= 0)
  {
    m_notifier_watch.setFd(-1, FdWatch::FD_WATCH_RD);
    close(fd);
  }
} /* CppDnsLookupWorker::abortLookup */


/****************************************************************************
 *
 * Private member functions
 *
 ****************************************************************************/

/*
 *----------------------------------------------------------------------------
 * Method:    CppDnsLookupWorker::workerFunc
 * Purpose:   This is the function that do the actual DNS lookup. It is
 *    	      started as a separate thread since res_nsearch is a
 *    	      blocking function.
 * Input:     ctx - A context containing query and result parameters
 * Output:    The answer and anslen variables in the ThreadContext will be
 *            filled in with the lookup result. The context will be returned
 *            from this function.
 * Author:    Tobias Blomberg
 * Created:   2021-07-14
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
CppDnsLookupWorker::ThreadContext CppDnsLookupWorker::workerFunc(
    CppDnsLookupWorker::ThreadContext ctx)
{
  std::ostream& th_cerr = ctx.thread_cerr;

  struct __res_state state;
  int ret = res_ninit(&state);
  if (ret != -1)
  {
    state.options = RES_DEFAULT;

    int qtype = 0;
    switch (ctx.type)
    {
      case DnsLookup::Type::A:
        qtype = ns_t_a;
        break;
      case DnsLookup::Type::PTR:
        qtype = ns_t_ptr;
        break;
      case DnsLookup::Type::CNAME:
        qtype = ns_t_cname;
        break;
      case DnsLookup::Type::SRV:
        qtype = ns_t_srv;
        break;
      default:
        assert(0);
    }

    const char *dname = ctx.label.c_str();
    ctx.anslen = res_nsearch(&state, dname, ns_c_in, qtype,
                             ctx.answer, NS_PACKETSZ);
    if (ctx.anslen == -1)
    {
      th_cerr << "*** ERROR: Name resolver failure -- res_nsearch: "
              << hstrerror(h_errno) << std::endl;
    }

      // FIXME: Valgrind complain about leaked memory in the resolver library
      //        when a lookup fails. It seems to be a one time leak though so it
      //        does not grow with every failed lookup. But even so, it seems
      //        that res_close is not cleaning up properly.
      //        Glibc 2.33-18 on Fedora 34.
    res_nclose(&state);
  }
  else
  {
    th_cerr << "*** ERROR: Name resolver failure -- res_ninit: "
            << hstrerror(h_errno) << std::endl;
  }

  close(ctx.notifier_wr);
  ctx.notifier_wr = -1;

  return std::move(ctx);
} /* CppDnsLookupWorker::workerFunc */


/*
 *----------------------------------------------------------------------------
 * Method:    CppDnsLookupWorker::notificationReceived
 * Purpose:   When the DNS lookup thread is done, this function will be
 *            called to parse the result and notify the user that an answer
 *            is available.
 * Input:     w - The file watch object (notification pipe)
 * Output:    None
 * Author:    Tobias Blomberg
 * Created:   2005-04-12
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
void CppDnsLookupWorker::notificationReceived(FdWatch *w)
{
  char buf[1];
  int cnt = read(w->fd(), buf, sizeof(buf));
  assert(cnt == 0);
  close(w->fd());
  w->setFd(-1, FdWatch::FD_WATCH_RD);

  const ThreadContext& ctx(m_result.get());

  const std::string& thread_errstr = ctx.thread_cerr.str();
  if (!thread_errstr.empty())
  {
    std::cerr << thread_errstr;
    setLookupFailed();
  }

  if (ctx.anslen == -1)
  {
    workerDone();
    return;
  }

  char errbuf[256];
  ns_msg msg;
  int ret = ns_initparse(ctx.answer, ctx.anslen, &msg);
  if (ret == -1)
  {
    strerror_r(ret, errbuf, sizeof(errbuf));
    std::cerr << "*** WARNING: ns_initparse failed: " << errbuf << std::endl;
    setLookupFailed();
    workerDone();
    return;
  }

  uint16_t msg_cnt = ns_msg_count(msg, ns_s_an);
  if (msg_cnt == 0)
  {
    setLookupFailed();
  }
  for (uint16_t rrnum=0; rrnum<msg_cnt; ++rrnum)
  {
    ns_rr rr;
    ret = ns_parserr(&msg, ns_s_an, rrnum, &rr);
    if (ret == -1)
    {
      strerror_r(errno, errbuf, sizeof(errbuf));
      std::cerr << "*** WARNING: DNS lookup failure in ns_parserr: "
                << errbuf << std::endl;
      setLookupFailed();
      continue;
    }
    const char *name = ns_rr_name(rr);
    uint16_t rr_class = ns_rr_class(rr);
    if (rr_class != ns_c_in)
    {
      std::cerr << "*** WARNING: Wrong RR class in DNS answer: "
                << rr_class << std::endl;
      setLookupFailed();
      continue;
    }
    uint32_t ttl = ns_rr_ttl(rr);
    uint16_t type = ns_rr_type(rr);
    const unsigned char *cp = ns_rr_rdata(rr);
    switch (type)
    {
      case ns_t_a:
      {
        struct in_addr in_addr;
        uint32_t ip = ns_get32(cp);
        in_addr.s_addr = ntohl(ip);
        addResourceRecord(
            new DnsResourceRecordA(name, ttl, IpAddress(in_addr)));
        break;
      }

      case ns_t_ptr:
      {
        char exp_dn[NS_MAXDNAME+1];
        ret = ns_name_uncompress(ns_msg_base(msg), ns_msg_end(msg), cp,
                                 exp_dn, NS_MAXDNAME);
        if (ret == -1)
        {
          strerror_r(errno, errbuf, sizeof(errbuf));
          std::cerr << "*** WARNING: DNS lookup failure in "
                       "ns_name_uncompress: " << errbuf << std::endl;
          setLookupFailed();
          continue;
        }
        size_t exp_dn_len = strlen(exp_dn);
        exp_dn[exp_dn_len] = '.';
        exp_dn[exp_dn_len+1] = 0;
        addResourceRecord(new DnsResourceRecordPTR(name, ttl, exp_dn));
        break;
      }

      case ns_t_cname:
      {
        char exp_dn[NS_MAXDNAME+1];
        ret = ns_name_uncompress(ns_msg_base(msg), ns_msg_end(msg), cp,
            exp_dn, NS_MAXDNAME);
        if (ret == -1)
        {
          strerror_r(errno, errbuf, sizeof(errbuf));
          std::cerr << "*** WARNING: DNS lookup failure in "
                       "ns_name_uncompress" << errbuf << std::endl;
          setLookupFailed();
          continue;
        }
        size_t exp_dn_len = strlen(exp_dn);
        exp_dn[exp_dn_len] = '.';
        exp_dn[exp_dn_len+1] = 0;
        addResourceRecord(new DnsResourceRecordCNAME(name, ttl, exp_dn));
        break;
      }

      case ns_t_srv:
      {
        unsigned int prio = ns_get16(cp);
        cp += NS_INT16SZ;
        unsigned int weight = ns_get16(cp);
        cp += NS_INT16SZ;
        unsigned int port = ns_get16(cp);
        cp += NS_INT16SZ;
        char exp_dn[NS_MAXDNAME+1];
        ret = ns_name_uncompress(ns_msg_base(msg), ns_msg_end(msg), cp,
            exp_dn, NS_MAXDNAME);
        if (ret == -1)
        {
          strerror_r(errno, errbuf, sizeof(errbuf));
          std::cerr << "*** WARNING: DNS lookup failure in "
                       "ns_name_uncompress: " << errbuf << std::endl;
          setLookupFailed();
          continue;
        }
        size_t exp_dn_len = strlen(exp_dn);
        exp_dn[exp_dn_len] = '.';
        exp_dn[exp_dn_len+1] = 0;
        addResourceRecord(
            new DnsResourceRecordSRV(name, ttl, prio, weight, port, exp_dn));
        break;
      }

      default:
        std::cerr << "*** WARNING: Unsupported RR type, " << type
                  << ", received in DNS query for " << name << std::endl;
        setLookupFailed();
        break;
    }
  }
  workerDone();
} /* CppDnsLookupWorker::notificationReceived */



/*
 * This file has not been truncated
 */

