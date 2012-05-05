//
// Replicated state machine implementation with a primary and several
// backups. The primary receives requests, assigns each a view stamp (a
// vid, and a sequence number) in the order of reception, and forwards
// them to all backups. A backup executes requests in the order that
// the primary stamps them and replies with an OK to the primary. The
// primary executes the request after it receives OKs from all backups,
// and sends the reply back to the client.
//
// The config module will tell the fab about a new view. If the
// primary in the previous view is a member of the new view, then it
// will stay the primary.  Otherwise, the smallest numbered node of
// the previous view will be the new primary.  In either case, the new
// primary will be a node from the previous view.  The configuration
// module constructs the sequence of views for the fab and the fab
// ensures there will be always one primary, who was a member of the
// last view.
//
// When a new node starts, the recovery thread is in charge of joining
// the fab.  It will collect the internal fab state from the primary;
// the primary asks the config module to add the new node and returns
// to the joining the internal fab state (e.g., paxos log). Since
// there is only one primary, all joins happen in well-defined total
// order.
//
// The recovery thread also runs during a view change (e.g, when a node
// has failed).  After a failure some of the backups could have
// processed a request that the primary has not, but those results are
// not visible to clients (since the primary responds).  If the
// primary of the previous view is in the current view, then it will
// be the primary and its state is authoritive: the backups download
// from the primary the current state.  A primary waits until all
// backups have downloaded the state.  Once the fab is in sync, the
// primary accepts requests again from clients.  If one of the backups
// is the new primary, then its state is authoritative.  In either
// scenario, the next view uses a node as primary that has the state
// resulting from processing all acknowledged client requests.
// Therefore, if the nodes sync up before processing the next request,
// the next view will have the correct state.
//
// While the fab in a view change (i.e., a node has failed, a new view
// has been formed, but the sync hasn't completed), another failure
// could happen, which complicates a view change.  During syncing the
// primary or backups can timeout, and initiate another Paxos round.
// There are 2 variables that fab uses to keep track in what state it
// is:
//    - inviewchange: a node has failed and the fab is performing a view change
//    - insync: this node is syncing its state
//
// If inviewchange is false and a node is the primary, then it can
// process client requests. If it is true, clients are told to retry
// later again.  While inviewchange is true, the fab may go through several
// member list changes, one by one.   After a member list
// change completes, the nodes tries to sync. If the sync complets,
// the view change completes (and inviewchange is set to false).  If
// the sync fails, the node may start another member list change
// (inviewchange = true and insync = false).
//
// The implementation should be used only with servers that run all
// requests run to completion; in particular, a request shouldn't
// block.  If a request blocks, the backup won't respond to the
// primary, and the primary won't execute the request.  A request may
// send an RPC to another host, but the RPC should be a one-way
// message to that host; the backup shouldn't do anything based on the
// response or execute after the response, because it is not
// guaranteed that all backup will receive the same response and
// execute in the same order.
//
// The implementation can be viewed as a layered system:
//       fab module     ---- in charge of replication
//       config module  ---- in charge of view management
//       Paxos module   ---- in charge of running Paxos to agree on a value
//
// Each module has threads and internal locks. Furthermore, a thread
// may call down through the layers (e.g., to run Paxos's proposer).
// When Paxos's acceptor accepts a new value for an instance, a thread
// will invoke an upcall to inform higher layers of the new value.
// The rule is that a module releases its internal locks before it
// upcalls, but can keep its locks when calling down.

#include <fstream>
#include <iostream>

#include "handle.h"
#include "fab.h"
#include "tprintf.h"
#include "lang/verify.h"
#include "fab_client.h"

static void *
recoverythread(void *x)
{
  fab *r = (fab *) x;
  r->recovery();
  return 0;
}

fab::fab(std::string _first, std::string _me) 
  : stf(0), primary(_first), insync (false), inviewchange (true), vid_commit(0),
    partitioned (false), dopartition(false), break1(false), break2(false)
{
  pthread_t th;

  last_myvs.vid = 0;
  last_myvs.seqno = 0;
  myvs = last_myvs;
  myvs.seqno = 1;

  pthread_mutex_init(&fab_mutex, NULL);
  pthread_mutex_init(&invoke_mutex, NULL);
  pthread_cond_init(&recovery_cond, NULL);
  pthread_cond_init(&sync_cond, NULL);

  cfg = new config(_first, _me, this);

  if (_first == _me) {
    // Commit the first view here. We can not have acceptor::acceptor
    // do the commit, since at that time this->cfg is not initialized
    commit_change(1);
  }
  fabrpc = cfg->get_rpcs();
  
	fabrpc->reg(fab_client_protocol::invoke, this, &fab::client_invoke);
  fabrpc->reg(fab_client_protocol::members, this, &fab::client_members);
  
	fabrpc->reg(fab_protocol::invoke, this, &fab::invoke);
  fabrpc->reg(fab_protocol::transferreq, this, &fab::transferreq);
  fabrpc->reg(fab_protocol::transferdonereq, this, &fab::transferdonereq);
  fabrpc->reg(fab_protocol::joinreq, this, &fab::joinreq);

  // tester must be on different port, otherwise it may partition itself
  testsvr = new rpcs(atoi(_me.c_str()) + 1);
  testsvr->reg(fab_test_protocol::net_repair, this, &fab::test_net_repairreq);
  testsvr->reg(fab_test_protocol::breakpoint, this, &fab::breakpointreq);

  {
      ScopedLock ml(&fab_mutex);
      VERIFY(pthread_create(&th, NULL, &recoverythread, (void *) this) == 0);
  }
}

void
fab::reg1(int proc, handler *h)
{
  ScopedLock ml(&fab_mutex);
  procs[proc] = h;
}

// The recovery thread runs this function
void
fab::recovery()
{
  bool r = true;
  ScopedLock ml(&fab_mutex);

  while (1) {
    while (!cfg->ismember(cfg->myaddr(), vid_commit)) {
      if (join(primary)) {
				tprintf("recovery: joined\n");
        commit_change_wo(cfg->vid());
      } else {
				VERIFY(pthread_mutex_unlock(&fab_mutex)==0);
				sleep (5); // XXX make another node in cfg primary?
				VERIFY(pthread_mutex_lock(&fab_mutex)==0);
      }
    }
    vid_insync = vid_commit;
    tprintf("recovery: sync vid_insync %d\n", vid_insync);
    if (primary == cfg->myaddr()) {
      r = sync_with_backups();
    } else {
      r = sync_with_primary();
    }
    tprintf("recovery: sync done\n");

    // If there was a commited viewchange during the synchronization, restart
    // the recovery
    if (vid_insync != vid_commit)
      continue;

    if (r) { 
      myvs.vid = vid_commit;
      myvs.seqno = 1;
      inviewchange = false;
    }
    tprintf("recovery: go to sleep %d %d\n", insync, inviewchange);
    pthread_cond_wait(&recovery_cond, &fab_mutex);
  }
}

bool
fab::sync_with_backups()
{
  pthread_mutex_unlock(&fab_mutex);
  {
    // Make sure that the state of lock_server_cache_fab is stable during 
    // synchronization; otherwise, the primary's state may be more recent
    // than replicas after the synchronization.
    ScopedLock ml(&invoke_mutex);
    // By acquiring and releasing the invoke_mutex once, we make sure that
    // the state of lock_server_cache_fab will not be changed until all
    // replicas are synchronized. The reason is that client_invoke arrives
    // after this point of time will see inviewchange == true, and returns
    // BUSY.
  }
  pthread_mutex_lock(&fab_mutex);

  backups.clear();
  std::vector<std::string> members = cfg->get_view(vid_insync);
  backups.insert(members.begin(), members.end());
  backups.erase(primary);

  // Start accepting synchronization request (statetransferreq) now!
  insync = true;
  // You fill this in for Lab 7
  // Wait until
  //   - all backups in view vid_insync are synchronized
  //   - or there is a committed viewchange

  while (vid_insync == vid_commit && !backups.empty()) {
    pthread_cond_wait(&recovery_cond, &fab_mutex);
  }

  insync = false;
  return backups.empty() && vid_insync == vid_commit;
}


bool
fab::sync_with_primary()
{
  // Remember the primary of vid_insync
  std::string m = primary;
  // You fill this in for Lab 7
  // Keep synchronizing with primary until the synchronization succeeds,
  // or there is a commited viewchange

  bool sync_success = false;

  while (vid_insync == vid_commit && !sync_success) {
    sync_success = statetransfer(m);
  }

  bool notify_success = false;
  if (sync_success) {
    notify_success = statetransferdone(m);
  }

  return sync_success && notify_success && vid_insync == vid_commit;
}


/**
 * Call to transfer state from m to the local node.
 * Assumes that fab_mutex is already held.
 */
bool
fab::statetransfer(std::string m)
{
	fab_protocol::transferres r;
  handle h(m);
  int ret;
  tprintf("fab::statetransfer: contact %s w. my last_myvs(%d,%d)\n", 
	 m.c_str(), last_myvs.vid, last_myvs.seqno);
  VERIFY(pthread_mutex_unlock(&fab_mutex)==0);
  rpcc *cl = h.safebind();
  if (cl) {
    ret = cl->call(fab_protocol::transferreq, cfg->myaddr(), 
                             last_myvs, vid_insync, r, rpcc::to(1000));
  }
  VERIFY(pthread_mutex_lock(&fab_mutex)==0);
  if (cl == 0 || ret != fab_protocol::OK) {
    tprintf("fab::statetransfer: couldn't reach %s %lx %d\n", m.c_str(), 
	   (long unsigned) cl, ret);
    return false;
  }
  if (stf && last_myvs != r.last) {
    stf->unmarshal_state(r.state);
  }
  last_myvs = r.last;
  tprintf("fab::statetransfer transfer from %s success, vs(%d,%d)\n", 
	 m.c_str(), last_myvs.vid, last_myvs.seqno);
  return true;
}

bool
fab::statetransferdone(std::string m) {
  // You fill this in for Lab 7
  // - Inform primary that this slave has synchronized for vid_insync

  int ret = 0;

  VERIFY(pthread_mutex_unlock(&fab_mutex) == 0);
  handle h(m);
  rpcc *cl = h.safebind();
  if (cl != NULL) {
    int r;
    ret = cl->call(fab_protocol::transferdonereq, cfg->myaddr(), vid_insync, 
		   r, rpcc::to(1000));
  }
  VERIFY(pthread_mutex_lock(&fab_mutex) == 0);

  return cl != NULL && ret == fab_protocol::OK;
}


bool
fab::join(std::string m) {
  handle h(m);
  int ret;
  fab_protocol::joinres r;

  tprintf("fab::join: %s mylast (%d,%d)\n", m.c_str(), last_myvs.vid, 
          last_myvs.seqno);
  VERIFY(pthread_mutex_unlock(&fab_mutex)==0);
  rpcc *cl = h.safebind();
  if (cl != 0) {
    ret = cl->call(fab_protocol::joinreq, cfg->myaddr(), last_myvs, 
		   r, rpcc::to(120000));
  }
  VERIFY(pthread_mutex_lock(&fab_mutex)==0);

  if (cl == 0 || ret != fab_protocol::OK) {
    tprintf("fab::join: couldn't reach %s %p %d\n", m.c_str(), 
	   cl, ret);
    return false;
  }
  tprintf("fab::join: succeeded %s\n", r.log.c_str());
  cfg->restore(r.log);
  return true;
}

/*
 * Config informs fab whenever it has successfully 
 * completed a view change
 */
void 
fab::commit_change(unsigned vid) 
{
  ScopedLock ml(&fab_mutex);
  commit_change_wo(vid);
}

void 
fab::commit_change_wo(unsigned vid) 
{
  if (vid <= vid_commit)
    return;
  tprintf("commit_change: new view (%d)  last vs (%d,%d) %s insync %d\n", 
	 vid, last_myvs.vid, last_myvs.seqno, primary.c_str(), insync);
  vid_commit = vid;
  inviewchange = true;
  set_primary(vid);
  pthread_cond_signal(&recovery_cond);
  if (cfg->ismember(cfg->myaddr(), vid_commit))
    breakpoint2();
}


void
fab::execute(int procno, std::string req, std::string &r)
{
  tprintf("execute\n");
  handler *h = procs[procno];
  VERIFY(h);
  unmarshall args(req);
  marshall rep;
  std::string reps;
  fab_protocol::status ret = h->fn(args, rep);
  marshall rep1;
  rep1 << ret;
  rep1 << rep.str();
  r = rep1.str();
}

//
// Clients call client_invoke to invoke a procedure on the replicated state
// machine: the primary receives the request, assigns it a sequence
// number, and invokes it on all members of the replicated state
// machine.
//
fab_client_protocol::status
fab::client_invoke(int procno, std::string req, std::string &r)
{
  // printf("in client_invoke %d\n", procno);

  // You fill this in for Lab 7
  ScopedLock ml(&invoke_mutex);
  // printf("acquired invoke mutex\n");
  pthread_mutex_lock(&fab_mutex);
  // printf("acquired fab mutex\n");

  int ret = fab_client_protocol::OK;

  if (inviewchange) {
    // printf("returning busy because in view change\n");
    // fflush(stdout);
    ret = fab_client_protocol::BUSY;
  } else if (primary != cfg->myaddr()) {
    // printf("returning notprimary because not primary\n");
    // fflush(stdout);
    ret = fab_client_protocol::NOTPRIMARY;
  } else {
    viewstamp cur_vs = myvs;
    last_myvs = myvs;
    ++myvs.seqno;

    // printf("in client_invoke %d before getview\n", procno);
    // fflush(stdout);

    bool all_ok = true;
    std::vector<std::string> cur_view = cfg->get_view(vid_commit);

    // printf("in client_invoke %d before loop\n", procno);
    // fflush(stdout);

    bool first_replica = true;
    for (unsigned int ii = 0; ii < cur_view.size() && all_ok; ++ii) {
      const std::string& cur_replica = cur_view[ii];
      if (cur_replica != primary) {

        VERIFY(pthread_mutex_unlock(&fab_mutex) == 0);

        fab_protocol::status ret;
        handle h(cur_replica);
        rpcc *cl = h.safebind();

        if (cl != NULL) {
          int dummy;
          ret = handle(cur_replica).safebind()->call(fab_protocol::invoke,
              procno, cur_vs, req, dummy, rpcc::to(1000));
        }

        VERIFY(pthread_mutex_lock(&fab_mutex) == 0);

        if (cl == NULL || ret != fab_protocol::OK) {
          // printf("setting allok false because of replica %s\n",
          //    cur_replica.c_str());
          // fflush(stdout);
          all_ok = false;
        }

        if (first_replica) {
          breakpoint1();
          first_replica = false;
          partition1();
        }
      }
    }

    if (all_ok) {
      execute(procno, req, r);
    } else {
      // printf("returning busy because not all ok\n");
      // fflush(stdout);
      ret = fab_client_protocol::BUSY;
    }
  }

  pthread_mutex_unlock(&fab_mutex);
  // printf("client_invoke returns %d\n", ret);
  // fflush(stdout);
  return ret;
}

// 
// The primary calls the internal invoke at each member of the
// replicated state machine 
//
// the replica must execute requests in order (with no gaps) 
// according to requests' seqno 

fab_protocol::status
fab::invoke(int proc, viewstamp vs, std::string req, int &dummy)
{
  // You fill this in for Lab 7
  ScopedLock ml(&invoke_mutex);
  ScopedLock rl(&fab_mutex);

  fab_protocol::status ret = fab_protocol::OK;

  if (inviewchange) {
    ret = fab_protocol::ERR;
  } else if (primary == cfg->myaddr()) {
    ret = fab_protocol::ERR;
  } else if (vs != myvs) {
    ret = fab_protocol::ERR;
  } else {
    last_myvs = myvs;
    ++myvs.seqno;
    std::string r;
    execute(proc, req, r);

    breakpoint1();
  }

  return ret;
}

/**
 * RPC handler: Send back the local node's state to the caller
 */
fab_protocol::status
fab::transferreq(std::string src, viewstamp last, unsigned vid, 
fab_protocol::transferres &r)
{
  ScopedLock ml(&fab_mutex);
  int ret = fab_protocol::OK;
  // Code will be provided in Lab 7
  tprintf("transferreq from %s (%d,%d) vs (%d,%d)\n", src.c_str(), 
	 last.vid, last.seqno, last_myvs.vid, last_myvs.seqno);
  if (!insync || vid != vid_insync) {
     return fab_protocol::BUSY;
  }
  if (stf && last != last_myvs) 
    r.state = stf->marshal_state();
  r.last = last_myvs;
  return ret;
}

/**
  * RPC handler: Inform the local node (the primary) that node m has synchronized
  * for view vid
  */
fab_protocol::status
fab::transferdonereq(std::string m, unsigned vid, int &)
{
  int ret = fab_protocol::OK;
  ScopedLock ml(&fab_mutex);
  // You fill this in for Lab 7
  // - Return BUSY if I am not insync, or if the slave is not synchronizing
  //   for the same view with me
  // - Remove the slave from the list of unsynchronized backups
  // - Wake up recovery thread if all backups are synchronized

  if (!insync || vid != vid_insync) {
    ret = fab_protocol::BUSY;
  } else {
    backups.erase(m);
    if (backups.empty()) {
      pthread_cond_broadcast(&recovery_cond);
    }
  }
  return ret;
}

// a node that wants to join an fab as a server sends a
// joinreq to the fab's current primary; this is the
// handler for that RPC.
fab_protocol::status
fab::joinreq(std::string m, viewstamp last, fab_protocol::joinres &r)
{
  int ret = fab_protocol::OK;

  ScopedLock ml(&fab_mutex);
  tprintf("joinreq: src %s last (%d,%d) mylast (%d,%d)\n", m.c_str(), 
	 last.vid, last.seqno, last_myvs.vid, last_myvs.seqno);
  if (cfg->ismember(m, vid_commit)) {
    tprintf("joinreq: is still a member\n");
    r.log = cfg->dump();
  } else if (cfg->myaddr() != primary) {
    tprintf("joinreq: busy\n");
    ret = fab_protocol::BUSY;
  } else {
    // We cache vid_commit to avoid adding m to a view which already contains 
    // m due to race condition
    unsigned vid_cache = vid_commit;
    VERIFY (pthread_mutex_unlock(&fab_mutex) == 0);
    bool succ = cfg->add(m, vid_cache);
    VERIFY (pthread_mutex_lock(&fab_mutex) == 0);
    if (cfg->ismember(m, cfg->vid())) {
      r.log = cfg->dump();
      tprintf("joinreq: ret %d log %s\n:", ret, r.log.c_str());
    } else {
      tprintf("joinreq: failed; proposer couldn't add %d\n", succ);
      ret = fab_protocol::BUSY;
    }
  }
  return ret;
}

/*
 * RPC handler: Send back all the nodes this local knows about to client
 * so the client can switch to a different primary 
 * when it existing primary fails
 */
fab_client_protocol::status
fab::client_members(int i, std::vector<std::string> &r)
{
  std::vector<std::string> m;
  ScopedLock ml(&fab_mutex);
  m = cfg->get_view(vid_commit);
  m.push_back(primary);
  r = m;
  tprintf("fab::client_members return %s m %s\n", print_members(m).c_str(),
	 primary.c_str());
  return fab_client_protocol::OK;
}

// if primary is member of new view, that node is primary
// otherwise, the lowest number node of the previous view.
// caller should hold fab_mutex
void
fab::set_primary(unsigned vid)
{
  std::vector<std::string> c = cfg->get_view(vid);
  std::vector<std::string> p = cfg->get_view(vid - 1);
  VERIFY (c.size() > 0);

  if (isamember(primary,c)) {
    tprintf("set_primary: primary stays %s\n", primary.c_str());
    return;
  }

  VERIFY(p.size() > 0);
  for (unsigned i = 0; i < p.size(); i++) {
    if (isamember(p[i], c)) {
      primary = p[i];
      tprintf("set_primary: primary is %s\n", primary.c_str());
      return;
    }
  }
  VERIFY(0);
}

bool
fab::amiprimary()
{
  ScopedLock ml(&fab_mutex);
  return primary == cfg->myaddr() && !inviewchange;
}


// Testing server

// Simulate partitions

// assumes caller holds fab_mutex
void
fab::net_repair_wo(bool heal)
{
  std::vector<std::string> m;
  m = cfg->get_view(vid_commit);
  for (unsigned i  = 0; i < m.size(); i++) {
    if (m[i] != cfg->myaddr()) {
        handle h(m[i]);
	tprintf("fab::net_repair_wo: %s %d\n", m[i].c_str(), heal);
	if (h.safebind()) h.safebind()->set_reachable(heal);
    }
  }
  fabrpc->set_reachable(heal);
}

fab_test_protocol::status 
fab::test_net_repairreq(int heal, int &r)
{
  ScopedLock ml(&fab_mutex);
  tprintf("fab::test_net_repairreq: %d (dopartition %d, partitioned %d)\n", 
	 heal, dopartition, partitioned);
  if (heal) {
    net_repair_wo(heal);
    partitioned = false;
  } else {
    dopartition = true;
    partitioned = false;
  }
  r = fab_test_protocol::OK;
  return r;
}

// simulate failure at breakpoint 1 and 2

void 
fab::breakpoint1()
{
  if (break1) {
    tprintf("Dying at breakpoint 1 in fab!\n");
    exit(1);
  }
}

void 
fab::breakpoint2()
{
  if (break2) {
    tprintf("Dying at breakpoint 2 in fab!\n");
    exit(1);
  }
}

void 
fab::partition1()
{
  if (dopartition) {
    net_repair_wo(false);
    dopartition = false;
    partitioned = true;
  }
}

fab_test_protocol::status
fab::breakpointreq(int b, int &r)
{
  r = fab_test_protocol::OK;
  ScopedLock ml(&fab_mutex);
  tprintf("fab::breakpointreq: %d\n", b);
  if (b == 1) break1 = true;
  else if (b == 2) break2 = true;
  else if (b == 3 || b == 4) cfg->breakpoint(b);
  else r = fab_test_protocol::ERR;
  return r;
}



