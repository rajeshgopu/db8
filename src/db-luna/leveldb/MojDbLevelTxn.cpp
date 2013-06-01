/* @@@LICENSE
*
* Copyright (c) 2013 LG Electronics
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include "db-luna/leveldb/MojDbLevelEngine.h"
#include "db-luna/leveldb/MojDbLevelTxn.h"
#include "db-luna/leveldb/defs.h"

// class MojDbLevelTableTxn
MojErr MojDbLevelTableTxn::begin(
            leveldb::DB* db,
            const leveldb::WriteOptions& options /* = leveldb::WriteOptions() */
            )
{
    // TODO: mutex and lock-file serialization
    m_db = db;
    m_writeOptions = options;
    return MojErrNone;
}

MojErr MojDbLevelTableTxn::abort()
{
    cleanup();
    return MojErrNone;
}

MojDbLevelTableTxn &MojDbLevelTableTxn::tableTxn(leveldb::DB *db)
{
    MojAssert( db ); // database should exist
    MojAssert( m_db == db ); // make sure we are within same database
    return *this;
}

void MojDbLevelTableTxn::Put(const leveldb::Slice& key,
                             const leveldb::Slice& val)
{
    m_writeBatch.Put(key, val);

    // populate local view
    std::string keyStr = key.ToString();

    m_pendingValues[keyStr] = val.ToString();
    m_pendingDeletes.erase(keyStr); // if hidden before
}

leveldb::Status MojDbLevelTableTxn::Get(const leveldb::Slice& key,
                                        std::string& val)
{
    std::string keyStr = key.ToString();

    // for keys deleted in this transaction
    if (m_pendingDeletes.count(keyStr) > 0)
    {
        return leveldb::Status::NotFound("Deleted inside transaction");
    }

    // for keys added in this transaction
    PendingValues::const_iterator it = m_pendingValues.find(key.ToString());
    if (it != m_pendingValues.end())
    {
        val = it->second;
        return leveldb::Status::OK();
    }

    // go directly to db
    return m_db->Get(leveldb::ReadOptions(), key, &val);
}

void MojDbLevelTableTxn::Delete(const leveldb::Slice& key)
{
    m_writeBatch.Delete(key);

    // populate local view
    std::string keyStr = key.ToString();

    m_pendingDeletes.insert(keyStr);
    m_pendingValues.erase(keyStr); // if set before

    // XXX: work around for delete by query
    // make this delete visible to cursors
    m_db->Delete(m_writeOptions, key);
}

MojErr MojDbLevelTableTxn::commitImpl()
{
    leveldb::Status s = m_db->Write(m_writeOptions, &m_writeBatch);
    MojLdbErrCheck(s, _T("db->Write"));

    cleanup();

    return MojErrNone;
}

void MojDbLevelTableTxn::cleanup()
{
    m_writeBatch.Clear();
    m_db = NULL;
    m_pendingValues.clear();
    m_pendingDeletes.clear();
}

// class MojDbLevelEnvTxn
MojErr MojDbLevelEnvTxn::begin(MojDbLevelEngine* eng)
{
    // TODO: mutex and lock-file serialization to implement strongest
    //       isolation level
    return MojErrNone;
}

MojErr MojDbLevelEnvTxn::abort()
{
    // Note creation of databases will not be rolled back
    return MojErrNone;
}

MojDbLevelTableTxn &MojDbLevelEnvTxn::tableTxn(leveldb::DB *db)
{
    // find within opened already
    for(TableTxns::iterator it = m_tableTxns.begin();
                            it != m_tableTxns.end();
                            ++it)
    {
        if ((*it)->db() == db) return *(*it);
    }

    // create new
    MojSharedPtr<MojDbLevelTableTxn> txn;
    txn.reset(new MojDbLevelTableTxn());
    txn->begin(db);
    m_tableTxns.push_back(txn);
    return *txn;
}

MojErr MojDbLevelEnvTxn::commitImpl()
{
    MojErr accErr = MojErrNone;
    for(TableTxns::iterator it = m_tableTxns.begin();
                            it != m_tableTxns.end();
                            ++it)
    {
        MojErr err = (*it)->commit();
        MojErrAccumulate(accErr, err);
    }
    return accErr;
}