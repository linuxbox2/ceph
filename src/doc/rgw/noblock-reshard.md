# Non-block Resharding

## Requirements

* Non-block resharding a bucket require upgraded to a supported release
    - Existing bucket reshard should be completed firstly before upgrading.

* Backward compatibility
    - If the rgw or rgw-admin nodes that do resharding upgrades to supported release but parts of osd nodes not, the reshard will be failed.
    - Only parts of osd nodes upgraded to a supported release and the rgw or rgw-admin nodes not, the reshard will execute blocked as before.

## Designing

Split the bucket resharding into two phases: logrecord and progress. A log will be written with index operation to src shards in first phase, and the client writes will not be blocked; we then block the client writes, go through the recording log and copy the changed index entries to dest shards in second phase. In this way, we can greatly reduce the time blocking client writes in resharding a bucket. 

The record log key is like `0x802001_gen_indexver.clsver.subop`, among it, the `gen` is stored in bucket_info with the same name, it accumulates in every reshard, so we can distinguish the log written in failed rehsharding; the clsver.subop can ensure that every key is distinct. 

The record log entry mainly contains idx, timestamp and sub_ver_traced fileds. The idx is same as the whole name of an omap key of index entry; the sub_ver_traced is used for determining whether the index entry recorded in log is written again, it tracks the `sub_ver` stored in `rgw_bucket_dir_entry` and `rgw_bucket_olh_entry`; and the timestamp is designed for deciding when to change the first phase of resharding to the second, becase in our future design, we go though log and copy index entries for several rounds until there only be few enough logs remained.

## Tasks

### Record Log

* The policy adopted here is not only recording a log for the entire write op, but for every change to the index entry. One op may correspond to multiple logs. In this way, the complexity of index synchronization can be reduced. You don't have to think about the details of operation. Especially with versioned objects, the same entry may involve multiple changes in a entire write or delete operation. Here, a log is recorded for each change. During data synchronization based on log, it tries to filter out repeated operations on the same entry.

### Copy Index Entries

* In logrecord state, copy inventoried index entries to dest shards and record a log for new writting entry.
 
* In progress state, block the writes, listing the logs written in logrecord state, then gain corresponding index entries and copy then to dest shards:
    - List logs written to src shards
    - Decode the log recorded and request corresponding index entry from src shard
    - Copy the index entry to the certain dest shard, if the index key decoded from log exists in dest shard but not in src shard, then delete it from dest shard too

### Bucket Stats

* In the progress state, some index entries that have already been copyed to dest shards in logrecord state may be copyed again, we should subtract their stats in dest shards firstly:
    - Request corresponding index entry from dest shard too based on log recorded
    - Get old stats of index entry if it exists in dest shard
    - Subtract the old entry stats of dest shard as adding stats of the new copyed one

### Reshard Logrecord Judge

When a bucket reshard faild in the logrecord phase, the reshard log should be stopped writen within a short time. To achieve it, we judge whether the resharding is executing properly in recording log once in the while, and the time is `rgw_reshard_progress_judge_interval`. If it has already failed, we clear resharding status and stop recording logs.

### Backward Compatibility

* The privious release only has one reshard phase: the progress phase which will block client writes. Because our release contains this phase and the process is same too, that means it is superset of privious release. So when privious rgw initiates a reshard, it will execute as before.

* When a updated rgw initiates a reshard, it firstly enter the logrecord phase which privious releases do not realized. That means the nodes which do not upgraded will deal with client write operations without recording logs. It may leads to part of these index entries missed. So we forbit this scene by controlling the cls_rgw_set_bucket_resharding_op version, then the updated rgw will stop resharding when meeting privious releases nodes.

## Future Prospects

* Add a timestamp already in recording log, we can go though logs recorded for several loops until the timestamp of all shards are near present time. When dealing with privoius logs, new logs will be recorded for comming writes. The blocked time will be cutted down to negligible in this way.
