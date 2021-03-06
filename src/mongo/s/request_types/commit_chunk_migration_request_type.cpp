/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/request_types/commit_chunk_migration_request_type.h"

#include "mongo/bson/util/bson_extract.h"

namespace mongo {
namespace {

const char kConfigSvrCommitChunkMigration[] = "_configsvrCommitChunkMigration";
const char kFromShard[] = "fromShard";
const char kToShard[] = "toShard";
const char kMigratedChunk[] = "migratedChunk";
const char kControlChunk[] = "controlChunk";
const char kFromShardCollectionVersion[] = "fromShardCollectionVersion";
const char kShardHasDistributedLock[] = "shardHasDistributedLock";

/**
 * Attempts to parse a ChunkRange from "field" in "source".
 */
StatusWith<ChunkRange> extractChunkRange(const BSONObj& source, StringData field) {
    BSONElement fieldElement;
    auto status = bsonExtractTypedField(source, field, BSONType::Object, &fieldElement);
    if (!status.isOK())
        return status;

    return ChunkRange::fromBSON(fieldElement.Obj());
}

/**
 * Attempts to parse a ShardId from "field" in "source".
 */
StatusWith<ShardId> extractShardId(const BSONObj& source, StringData field) {
    std::string stringResult;

    auto status = bsonExtractStringField(source, field, &stringResult);
    if (!status.isOK()) {
        return status;
    }

    if (stringResult.empty()) {
        return Status(ErrorCodes::UnsupportedFormat,
                      "The field '" + field.toString() + "' cannot be empty");
    }

    return ShardId(stringResult);
}

}  // namespace

StatusWith<CommitChunkMigrationRequest> CommitChunkMigrationRequest::createFromCommand(
    const NamespaceString& nss, const BSONObj& obj) {

    auto migratedChunkRange = extractChunkRange(obj, kMigratedChunk);
    if (!migratedChunkRange.isOK()) {
        return migratedChunkRange.getStatus();
    }

    CommitChunkMigrationRequest request(nss, std::move(migratedChunkRange.getValue()));

    {
        auto fromShard = extractShardId(obj, kFromShard);
        if (!fromShard.isOK()) {
            return fromShard.getStatus();
        }

        request._fromShard = std::move(fromShard.getValue());
    }

    {
        auto toShard = extractShardId(obj, kToShard);
        if (!toShard.isOK()) {
            return toShard.getStatus();
        }

        request._toShard = std::move(toShard.getValue());
    }

    {
        // controlChunk is optional, so parse it if present.
        if (obj.hasField(kControlChunk)) {
            auto controlChunkRange = extractChunkRange(obj, kControlChunk);
            if (!controlChunkRange.isOK()) {
                return controlChunkRange.getStatus();
            }

            request._controlChunkRange = std::move(controlChunkRange.getValue());
        }
    }

    {
        auto statusWithChunkVersion =
            ChunkVersion::parseFromBSONWithFieldForCommands(obj, kFromShardCollectionVersion);
        if (statusWithChunkVersion.isOK()) {
            request._fromShardCollectionVersion = std::move(statusWithChunkVersion.getValue());
        } else if (statusWithChunkVersion != ErrorCodes::NoSuchKey) {
            return statusWithChunkVersion.getStatus();
        }
    }

    {
        Status shardHasDistLockStatus = bsonExtractBooleanField(
            obj, kShardHasDistributedLock, &request._shardHasDistributedLock);
        if (!shardHasDistLockStatus.isOK()) {
            return shardHasDistLockStatus;
        }
    }

    return request;
}

void CommitChunkMigrationRequest::appendAsCommand(
    BSONObjBuilder* builder,
    const NamespaceString& nss,
    const ShardId& fromShard,
    const ShardId& toShard,
    const ChunkType& migratedChunkType,
    const boost::optional<ChunkType>& controlChunkType,
    const ChunkVersion& fromShardCollectionVersion,
    const bool& shardHasDistributedLock) {
    invariant(builder->asTempObj().isEmpty());
    invariant(nss.isValid());

    builder->append(kConfigSvrCommitChunkMigration, nss.ns());
    builder->append(kFromShard, fromShard.toString());
    builder->append(kToShard, toShard.toString());
    builder->append(kMigratedChunk, migratedChunkType.toBSON());
    fromShardCollectionVersion.appendWithFieldForCommands(builder, kFromShardCollectionVersion);
    builder->append(kShardHasDistributedLock, shardHasDistributedLock);

    if (controlChunkType) {
        builder->append(kControlChunk, controlChunkType->toBSON());
    }
}

const ChunkRange& CommitChunkMigrationRequest::getControlChunkRange() const {
    invariant(_controlChunkRange);
    return _controlChunkRange.get();
}

CommitChunkMigrationRequest::CommitChunkMigrationRequest(const NamespaceString& nss,
                                                         const ChunkRange& range)
    : _nss(nss), _migratedChunkRange(range) {}

}  // namespace mongo
