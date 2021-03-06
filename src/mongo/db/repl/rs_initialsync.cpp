/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rs.h"

#include "mongo/bson/optime.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/client.h"
#include "mongo/db/cloner.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/initial_sync.h"
#include "mongo/db/repl/initial_sync.h"
#include "mongo/db/repl/member.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace repl {

    using namespace mongoutils;

    // add try/catch with sleep

    void isyncassert(const string& msg, bool expr) {
        if( !expr ) {
            string m = str::stream() << "initial sync " << msg;
            theReplSet->sethbmsg(m, 0);
            uasserted(13404, m);
        }
    }

    void ReplSetImpl::syncDoInitialSync() {
        static const int maxFailedAttempts = 10;

        OperationContextImpl txn;
        createOplog(&txn);

        int failedAttempts = 0;
        while ( failedAttempts < maxFailedAttempts ) {
            try {
                _initialSync();
                break;
            }
            catch(DBException& e) {
                failedAttempts++;
                str::stream msg;
                msg << "initial sync exception: ";
                msg << e.toString() << " " << (maxFailedAttempts - failedAttempts) << " attempts remaining" ;
                sethbmsg(msg, 0);
                sleepsecs(30);
            }
        }
        fassert( 16233, failedAttempts < maxFailedAttempts);
    }

    bool ReplSetImpl::_initialSyncClone(OperationContext* txn,
                                        Cloner& cloner,
                                        const std::string& host,
                                        const list<string>& dbs,
                                        bool dataPass) {

        for( list<string>::const_iterator i = dbs.begin(); i != dbs.end(); i++ ) {
            const string db = *i;
            if( db == "local" ) 
                continue;
            
            if ( dataPass )
                sethbmsg( str::stream() << "initial sync cloning db: " << db , 0);
            else
                sethbmsg( str::stream() << "initial sync cloning indexes for : " << db , 0);

            string err;
            int errCode;
            CloneOptions options;
            options.fromDB = db;
            options.logForRepl = false;
            options.slaveOk = true;
            options.useReplAuth = true;
            options.snapshot = false;
            options.mayYield = true;
            options.mayBeInterrupted = false;
            options.syncData = dataPass;
            options.syncIndexes = ! dataPass;

            // Make database stable
            Lock::DBWrite dbWrite(txn->lockState(), db);

            if (!cloner.go(txn, db, host, options, NULL, err, &errCode)) {
                sethbmsg(str::stream() << "initial sync: error while "
                                       << (dataPass ? "cloning " : "indexing ") << db
                                       << ".  " << (err.empty() ? "" : err + ".  ")
                                       << "sleeping 5 minutes" ,0);
                return false;
            }
        }

        return true;
    }

    static void emptyOplog(OperationContext* txn) {
        Client::WriteContext ctx(txn, rsoplog);

        Collection* collection = ctx.ctx().db()->getCollection(txn, rsoplog);

        // temp
        if( collection->numRecords(txn) == 0 )
            return; // already empty, ok.

        LOG(1) << "replSet empty oplog" << rsLog;
        uassertStatusOK( collection->truncate(txn) );
        ctx.commit();
    }

    const Member* ReplSetImpl::getMemberToSyncTo() {
        lock lk(this);

        // if we have a target we've requested to sync from, use it

        if (_forceSyncTarget) {
            Member* target = _forceSyncTarget;
            _forceSyncTarget = 0;
            sethbmsg( str::stream() << "syncing to: " << target->fullName() << " by request", 0);
            return target;
        }

        const Member* primary = box.getPrimary();

        // wait for 2N pings before choosing a sync target
        if (_cfg) {
            int needMorePings = config().members.size()*2 - HeartbeatInfo::numPings;

            if (needMorePings > 0) {
                OCCASIONALLY log() << "waiting for " << needMorePings << " pings from other members before syncing" << endl;
                return NULL;
            }

            // If we are only allowed to sync from the primary, return that
            if (!_cfg->chainingAllowed()) {
                // Returns NULL if we cannot reach the primary
                return primary;
            }
        }

        // find the member with the lowest ping time that has more data than me

        // Find primary's oplog time. Reject sync candidates that are more than
        // maxSyncSourceLagSecs seconds behind.
        OpTime primaryOpTime;
        if (primary)
            primaryOpTime = primary->hbinfo().opTime;
        else
            // choose a time that will exclude no candidates, since we don't see a primary
            primaryOpTime = OpTime(maxSyncSourceLagSecs, 0);

        if (primaryOpTime.getSecs() < static_cast<unsigned int>(maxSyncSourceLagSecs)) {
            // erh - I think this means there was just a new election
            // and we don't yet know the new primary's optime
            primaryOpTime = OpTime(maxSyncSourceLagSecs, 0);
        }

        OpTime oldestSyncOpTime(primaryOpTime.getSecs() - maxSyncSourceLagSecs, 0);

        Member *closest = 0;
        time_t now = 0;

        // Make two attempts.  The first attempt, we ignore those nodes with
        // slave delay higher than our own.  The second attempt includes such
        // nodes, in case those are the only ones we can reach.
        // This loop attempts to set 'closest'.
        for (int attempts = 0; attempts < 2; ++attempts) {
            for (Member *m = _members.head(); m; m = m->next()) {
                if (!m->syncable())
                    continue;

                if (m->state() == MemberState::RS_SECONDARY) {
                    // only consider secondaries that are ahead of where we are
                    if (m->hbinfo().opTime <= lastOpTimeWritten)
                        continue;
                    // omit secondaries that are excessively behind, on the first attempt at least.
                    if (attempts == 0 &&
                        m->hbinfo().opTime < oldestSyncOpTime)
                        continue;
                }

                // omit nodes that are more latent than anything we've already considered
                if (closest &&
                    (m->hbinfo().ping > closest->hbinfo().ping))
                    continue;

                if (attempts == 0 &&
                    (myConfig().slaveDelay < m->config().slaveDelay || m->config().hidden)) {
                    continue; // skip this one in the first attempt
                }

                map<string,time_t>::iterator vetoed = _veto.find(m->fullName());
                if (vetoed != _veto.end()) {
                    // Do some veto housekeeping
                    if (now == 0) {
                        now = time(0);
                    }

                    // if this was on the veto list, check if it was vetoed in the last "while".
                    // if it was, skip.
                    if (vetoed->second >= now) {
                        if (time(0) % 5 == 0) {
                            log() << "replSet not trying to sync from " << (*vetoed).first
                                  << ", it is vetoed for " << ((*vetoed).second - now) << " more seconds" << rsLog;
                        }
                        continue;
                    }
                    _veto.erase(vetoed);
                    // fall through, this is a valid candidate now
                }
                // This candidate has passed all tests; set 'closest'
                closest = m;
            }
            if (closest) break; // no need for second attempt
        }

        if (!closest) {
            return NULL;
        }

        sethbmsg( str::stream() << "syncing to: " << closest->fullName(), 0);

        return closest;
    }

    void ReplSetImpl::veto(const string& host, const unsigned secs) {
        lock lk(this);
        _veto[host] = time(0)+secs;
    }

    /**
     * Replays the sync target's oplog from lastOp to the latest op on the sync target.
     *
     * @param syncer either initial sync (can reclone missing docs) or "normal" sync (no recloning)
     * @param r      the oplog reader
     * @param source the sync target
     * @return if applying the oplog succeeded
     */
    bool ReplSetImpl::_initialSyncApplyOplog( OperationContext* ctx,
                                              repl::SyncTail& syncer,
                                              OplogReader* r,
                                              const Member* source) {
        const OpTime startOpTime = lastOpTimeWritten;
        BSONObj lastOp;
        try {
            // It may have been a long time since we last used this connection to
            // query the oplog, depending on the size of the databases we needed to clone.
            // A common problem is that TCP keepalives are set too infrequent, and thus
            // our connection here is terminated by a firewall due to inactivity.
            // Solution is to increase the TCP keepalive frequency.
            lastOp = r->getLastOp(rsoplog);
        } catch ( SocketException & ) {
            log() << "connection lost to " << source->h().toString() << "; is your tcp keepalive interval set appropriately?";
            if( !r->connect(source->h()) ) {
                sethbmsg( str::stream() << "initial sync couldn't connect to " << source->h().toString() , 0);
                throw;
            }
            // retry
            lastOp = r->getLastOp(rsoplog);
        }

        isyncassert( "lastOp is empty ", !lastOp.isEmpty() );

        OpTime stopOpTime = lastOp["ts"]._opTime();

        // If we already have what we need then return.
        if (stopOpTime == startOpTime)
            return true;

        verify( !stopOpTime.isNull() );
        verify( stopOpTime > startOpTime );

        // apply till stopOpTime
        try {
            syncer.oplogApplication(ctx, stopOpTime);
        }
        catch (const DBException&) {
            log() << "replSet initial sync failed during oplog application phase, and will retry"
                  << rsLog;

            lastOpTimeWritten = OpTime();
            lastH = 0;

            sleepsecs(5);
            return false;
        }
        
        return true;
    }

    /**
     * Do the initial sync for this member.  There are several steps to this process:
     *
     *     0. Add _initialSyncFlag to minValid collection to tell us to restart initial sync if we
     *        crash in the middle of this procedure
     *     1. Record start time.
     *     2. Clone.
     *     3. Set minValid1 to sync target's latest op time.
     *     4. Apply ops from start to minValid1, fetching missing docs as needed.
     *     5. Set minValid2 to sync target's latest op time.
     *     6. Apply ops from minValid1 to minValid2.
     *     7. Build indexes.
     *     8. Set minValid3 to sync target's latest op time.
     *     9. Apply ops from minValid2 to minValid3.
          10. Cleanup minValid collection: remove _initialSyncFlag field, set ts to minValid3 OpTime
     *
     * At that point, initial sync is finished.  Note that the oplog from the sync target is applied
     * three times: step 4, 6, and 8.  4 may involve refetching, 6 should not.  By the end of 6,
     * this member should have consistent data.  8 is "cosmetic," it is only to get this member
     * closer to the latest op time before it can transition out of startup state
     */
    void ReplSetImpl::_initialSync() {
        InitialSync init(BackgroundSync::get());
        SyncTail tail(BackgroundSync::get());
        sethbmsg("initial sync pending",0);

        // if this is the first node, it may have already become primary
        if ( box.getState().primary() ) {
            sethbmsg("I'm already primary, no need for initial sync",0);
            return;
        }

        const Member *source = getMemberToSyncTo();
        if (!source) {
            sethbmsg("initial sync need a member to be primary or secondary to do our initial sync", 0);
            sleepsecs(15);
            return;
        }

        string sourceHostname = source->h().toString();
        init.setHostname(sourceHostname);
        OplogReader r;
        if( !r.connect(source->h()) ) {
            sethbmsg( str::stream() << "initial sync couldn't connect to " << source->h().toString() , 0);
            sleepsecs(15);
            return;
        }

        BSONObj lastOp = r.getLastOp(rsoplog);
        if( lastOp.isEmpty() ) {
            sethbmsg("initial sync couldn't read remote oplog", 0);
            sleepsecs(15);
            return;
        }

        OperationContextImpl txn;

        if (getGlobalReplicationCoordinator()->getSettings().fastsync) {
            log() << "fastsync: skipping database clone" << rsLog;

            // prime oplog
            init.syncApply(&txn, lastOp, false);
            _logOpObjRS(&txn, lastOp);
            return;
        }
        else {
            // Add field to minvalid document to tell us to restart initial sync if we crash
            theReplSet->setInitialSyncFlag(&txn);

            sethbmsg("initial sync drop all databases", 0);
            dropAllDatabasesExceptLocal(&txn);

            sethbmsg("initial sync clone all databases", 0);

            list<string> dbs = r.conn()->getDatabaseNames();

            Cloner cloner;
            if (!_initialSyncClone(&txn, cloner, r.conn()->getServerAddress(), dbs, true)) {
                veto(source->fullName(), 600);
                sleepsecs(300);
                return;
            }

            sethbmsg("initial sync data copy, starting syncup",0);

            // prime oplog
            init.syncApply(&txn, lastOp, false);
            _logOpObjRS(&txn, lastOp);

            log() << "oplog sync 1 of 3" << endl;
            if (!_initialSyncApplyOplog(&txn, init, &r , source)) {
                return;
            }

            // Now we sync to the latest op on the sync target _again_, as we may have recloned ops
            // that were "from the future" compared with minValid. During this second application,
            // nothing should need to be recloned.
            log() << "oplog sync 2 of 3" << endl;
            if (!_initialSyncApplyOplog(&txn, init, &r , source)) {
                return;
            }
            // data should now be consistent

            sethbmsg("initial sync building indexes",0);
            if (!_initialSyncClone(&txn, cloner, r.conn()->getServerAddress(), dbs, false)) {
                veto(source->fullName(), 600);
                sleepsecs(300);
                return;
            }
        }

        log() << "oplog sync 3 of 3" << endl;
        if (!_initialSyncApplyOplog(&txn, tail, &r, source)) {
            return;
        }
        
        // ---------

        Status status = getGlobalAuthorizationManager()->initialize(&txn);
        if (!status.isOK()) {
            warning() << "Failed to reinitialize auth data after initial sync. " << status;
            return;
        }

        sethbmsg("initial sync finishing up",0);

        verify( !box.getState().primary() ); // wouldn't make sense if we were.

        {
            Client::WriteContext cx(&txn, "local.");

            try {
                log() << "replSet set minValid=" << lastOpTimeWritten << rsLog;
            }
            catch(...) { }

            // Initial sync is now complete.  Flag this by setting minValid to the last thing
            // we synced.
            theReplSet->setMinValid(&txn, lastOpTimeWritten);

            // Clear the initial sync flag.
            theReplSet->clearInitialSyncFlag(&txn);
            cx.commit();
        }
        {
            boost::unique_lock<boost::mutex> lock(theReplSet->initialSyncMutex);
            theReplSet->initialSyncRequested = false;
        }

        // If we just cloned & there were no ops applied, we still want the primary to know where
        // we're up to
        BackgroundSync::notify();

        changeState(MemberState::RS_RECOVERING);
        sethbmsg("initial sync done",0);
    }

} // namespace repl
} // namespace mongo
