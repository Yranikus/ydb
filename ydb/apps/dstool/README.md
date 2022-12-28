# How to do things with ydb-dstool

### Get available commands

In order to list all available commands along with their descriptions in a nicely printed tree run

```
ydb-dstool --help
```

### Get help for a particular subset of commands or a command

```
ydb-dstool pdisk --help
```

The above command prints help for the ```pdisk``` commands.

```
ydb-dstool pdisk list --help
```

The above command prints help for the ```pdisk list``` command.

### Make command operation verbose

To make operation of a command verbose add ```--verbose``` to global options:

```
ydb-dstool --verbose -e ydbd.endpoint vdisk evict --vdisk-ids ${vdisk_id}
```

### Don't show non-vital messages

To dismiss non-vital messages of a command add ```--quiet``` to global options:

```
ydb-dstool --quiet -e ydbd.endpoint pool balance
```

### Run command without side effects

To run command without side effect add ```--dry-run``` to global options:

```
ydb-dstool --dry-run -e ydbd.endpoint vdisk evict --vdisk-ids ${vdisk_id}
```

### Handle errors

By convention ```ydb-dstool``` returns 0 on success, and non-zero on failure. You can check exit status
as follows:

```
~$ ydb-dstool -e ydbd.endpoint vdisk evict --vdisk-ids ${vdisk_id}
~$ if [ $? -eq 0 ]; then echo "success"; else echo "failure"; fi
```

```ydb-dstool``` outputs errors to ```stderr``` so to redirect errors to ```errors.txt``` one could run:

```
~$ ydb-dstool -e ydbd.endpoint vdisk evict --vdisk-ids ${vdisk_id} 2> ~/errors.txt
```

### Set endpoint

Еndpoint is a connection point used to perform operations on cluster. It is set by a triplet
```[PROTOCOL://]HOST[:PORT]```. To set endpoint use ```--endpoint``` global option:

```
ydb-dstool --endpoint https://ydbd.endpoint:8765 pdisk list
```

The endpoint's protocol from the above command is ```https```, host is ```ydbd.endpoint```, port is ```8765```.

### Set authentication token

There is support for authentication with access token. When authentication is required, user can set authentication
token with ```--token-file``` global option:

```
ydb-dstool -e ydbd.endpoint --token-file ~/access_token
```

The above command reads ```~/access_token``` and uses it's contents as an access token for authentication.

### Set output format

Output format can be set by the ```--format``` command option. The following formats
are available:

1. ```pretty``` (default)
2. ```tsv``` (available mainly for list commands)
3. ```csv``` (available mainly for list commands)
4. ```json```

To set output format to ```tsv``` add ```--format tsv``` to command options:

```
ydb-dstool -e ydbd.endpoint pdisk list --format tsv
```

### Exclude header from the output

To exclude header with the column names from the output add ```--no-header``` to command options:

```
ydb-dstool -e ydbd.endpoint pdisk list --format tsv --no-header
```

### Output all available columns

By default a listing like command outputs only certain columns. The default columns vary from command to command.
To output all available columns add ```--all-columns``` to command options:

```
ydb-dstool -e ydbd.endpoint pdisk list --all-columns
```

### Output only certain columns

To output only certain columns add ```--columns``` along with a space separated list of columns names to command
options:

```
ydb-dstool -e ydbd.endpoint pdisk list --columns NodeId:PDiskId Path
```

The above command lists only the ```NodeId:PDiskId```, ```Path``` columns while listing pdisks.

### Sort output by certain columns

To sort output by certain columns add ```--sort-by``` along with a space separated list of columns names to command
options:

```
ydb-dstool -e ydbd.endpoint pdisk list --sort-by FQDN
```

The above command lists pdisks sorted by the ```FQDN``` column.

## Do things with pdisks

### List pdisks

```
ydb-dstool -e ydbd.endpoint pdisk list
```

The above command lists all pdisks of a cluster along with their state.

### Show space usage of every pdisk

```
ydb-dstool -e ydbd.endpoint pdisk list --show-pdisk-usage --human-readable
```

The above command lists usage of all pdisks of a cluster in a human-readable way.

### Prevent new groups from using certain pdisks

```
ydb-dstool -e ydbd.endpoint pdisk set --decommit-status DECOMMIT_PENDING --pdisk-ids "[NODE_ID:PDISK_ID]"
```

The above command prevents new groups from using pdisk ```"[NODE_ID:PDISK_ID]"```.

### Move data out from certain pdisks

```
ydb-dstool -e ydbd.endpoint pdisk set --decommit-status DECOMMIT_IMMINENT --pdisk-ids "[NODE_ID:PDISK_ID]"
```

The above command initiates a background process that is going to move all of the data from
pdisk ```"[NODE_ID:PDISK_ID]"``` to some ```DECOMMIT_NONE``` pdisks. It's useful, for example, to accomplish
this step prior to unplugging either certain disks or complete host from a cluster.

## Do things with vdisks

### List vdisks

```
ydb-dstool -e ydbd.endpoint vdisk list
```

The above command lists all vdisks of a cluster along with the corresponding pdisks.

### Show status of pdisks were vdisks reside

```
ydb-dstool -e ydbd.endpoint vdisk list --show-pdisk-status
```

The above command lists all vdisks of a cluster along with the corresponding pdisks. On top of that, for every
vdisk it lists the status of the corresponding pdisk where vdisk resides.

### Show space usage every vdisk

```
ydb-dstool -e ydbd.endpoint vdisk list --show-vdisk-usage --human-readable
```

The above command lists usage of all vdisks of a cluster in a human-readable way.

### Unload certain pdisks by moving some vdisks from them

```
ydb-dstool -e ydbd.endpoint vdisk evict --vdisk-ids "[8200001b:3:0:7:0] [8200001c:1:0:1:0]"
```

The above command evicts vdisks ```[8200001b:3:0:7:0]```, ```[8200001c:1:0:1:0]``` from their current pdisks to
some other pdisks in the cluster. This command is useful when certain pdisks are unable to cope with the load or
are running out of space. This might happen because of usage sckew of certain groups.

### Wipe certain vdisks

```
ydb-dstool -e ydbd.endpoint vdisk wipe --vdisk-ids "[8200001b:3:0:7:0] [8200001c:1:0:1:0]" --run
```

The above command wipes out vdisks ```[8200001b:3:0:7:0]```, ```[8200001c:1:0:1:0]```. This command is useful when
vdisk becomes unhealable.

### Remove no longer needed donor vdisks

```
ydb-dstool -e ydbd.endpoint vdisk remove-donor --vdisk-ids "[8200001b:3:0:7:0] [8200001c:1:0:1:0]"
````

The above command removes donor vdisks ```[8200001b:3:0:7:0]```, ```[8200001c:1:0:1:0]```. The provided vdisks
have to be in donor state.

## Do things with groups

### List groups

```
ydb-dstool -e ydbd.endpoint group list
```

The above command lists all groups of a cluster.

### Show aggregated statuses of vdisks within group

To show aggregated statuses of vdisks within a group (i.e. how many vdisks within a group are in a certain state),
add ```--show-vdisk-status``` to command options:

```
ydb-dstool -e ydbd.endpoint group list --show-vdisk-status
```

### Show space usage of every group

To show space usage of groups, add ```--show-vdisk-usage``` to command options:

```
ydb-dstool -e ydbd.endpoint group list --show-vdisk-usage -H
```

The above command lists all groups of a cluster along with their space usage in a human-readable way.

### Check certain groups for compliance with failure model

```
ydb-dstool -e ydbd.endpoint group check --group-ids 2181038097 2181038105 --failure-model
```

The above command checks groups ```2181038097```, ```2181038105``` for compliance with their failure model.

### Show space usage of groups by tablets

```
ydb-dstool -e ydbd.endpoint group show usage-by-tablets
```

The above command shows which tablets are using which groups and what the space usage is.

### Show info about certain blob from a certain group

```
ydb-dstool -e ydbd.endpoint group show blob-info --group-id 2181038081 --blob-id "[72075186224037892:1:2:1:8192:410:0]"
```

The above command shows information about blob ```[72075186224037892:1:2:1:8192:410:0]``` that is stored in
group ```2181038081```. This command might be useful in certain debug scenarios.

### Add new groups to certain pool

```
ydb-dstool -e ydbd.endpoint group add --pool-name /Root:nvme --groups 1
```

The above command adds one group to the pool ```/Root:nvme```

### Figure out whether certain number of groups can be added

```
ydb-dstool --dry-run -e ydbd.endpoint group add --pool-name /Root:nvme --groups 10
```

The above command adds ten groups to the pool ```/Root:nvme``` without actually adding them. It might be useful
in capacity assesment scenarios.

## Do things with pools

Pool is a collection of groups.

### List pools

```
ydb-dstool -e ydbd.endpoint pool list
```

The above command lists all pools of a cluster.

### Show aggregated statuses of groups within pool

To show aggregated statuses of groups within a pool (i.e. how many groups within a pool are in a certain state),
add ```--show-group-status``` to command options:

```
ydb-dstool -e ydbd.endpoint pool list --show-group-status
```

### Show aggregated statuses of vdisks within pool

To show aggregated statuses of vdisks within a pool (i.e. how many vdisks within a pool are in a certain state),
add ```--show-vdisk-status``` to command options:

```
ydb-dstool -e ydbd.endpoint pool list --show-vdisk-status
```

### Show space usage of pools

To show space usage of pools, add ```--show-vdisk-usage``` to command options:

```
ydb-dstool -e ydbd.endpoint pool list --show-vdisk-usage -H
```

The above command lists all pools of a cluster along with their space usage in a human-readable way.

### Show estimated space usage of pools

TODO

## Do things with boxes

Box is a collection of pdisks.

###  List boxes

```
ydb-dstool -e ydbd.endpoint box list

```

The above command lists all boxes of a cluster.

### Show aggregated statuses of pdisks within box

Tow show aggregated statuses of pdisks within a box (i.e. how many pdisks within a box are in a certain state),
add ```--show-pdisk-status``` to command options:

```
ydb-dstool -e ydbd.endpoint box list --show-pdisk-status
```

### Show space usage of boxes

To show space usage of boxes, add ```--show-pdisk-usage``` to command options:

```
ydb-dstool -e ydbd.endpoint box list --show-pdisk-usage -H
```

The above command lists all boxes of a cluster along with their space usage in a human-readable way.

## Do things with nodes

A node is a basic working unit in a YDB cluster. The basic building blocks like pdisk and vdisk are run on nodes.
In terms of implementation, a node is a a YDB process running on one of cluster's machines.

### List nodes

```
ydb-dstool -e ydbd.endpoint node list
```

The above command lists all nodes of a cluster.

## Do things with a cluster as a whole

### Show how many cluster entities there are

```
ydb-dstool -e ydbd.endpoint cluster list
```

The above command shows how many

* hosts
* nodes
* pools
* groups
* vdisks
* boxes
* pdisks

are in the cluster.

### Move vdisks out from overpopulated pdisks

In rare cases some pdisks can become overpopulated (i.e. they host too many vdisks) and the cluster would benefit from
balancing of vdisks over pdisks. To accomplish this, run the following command:

```
ydb-dstool -e ydbd.endpoint cluster balance
```

The above command moves out vdisks from overpopulated pdisks. A single vdisk is moved at a time so that the failure model of
the respective group doesn't brake.

### Enable/Disable self-healing

Sometimes disks or even nodes fail which impacts vdisks that reside on them. As a result failure model of impacted groups
acquires one of the following statuses:

* PARTIAL (some vdisks within the group don't function, but failure model allows some more failures within the group)
* DEGRADED (loss of one more vdisk within the group will make the group DISINTEGRATED)
* DISINTEGRATED (group can't process read/write requests)

Self-healing enables automatic eviction of vdisks along with the neccessary data recovery for groups where there is a single
failed vdisk within a group.

To enable self-healing on a cluster, run the following command:

```
ydb-dstool -e ydbd.endpoint cluster set --enable-self-heal
```

To disable self-healing on a cluster, run the following command:

```
ydb-dstool -e ydbd.endpoint cluster set --disable-self-heal
```

### Enable/Disable donors

When vdisk in a group is substituted, the new vdisk needs to recover all of the data, located on the old vdisk, from the remaining
vdisks of the group. The bigger the old vdisk, the more time and resources recovery takes. In order to alleviate this process, the
old vdisk could be used as a donor, so that the new vdisk would copy all of the data from the old vdisk.

To enable support for donor vdisk mode on a cluster, run the following command:

```
ydb-dstool -e ydbd.endpoint cluster set --enable-donor-mode
```

To disable support for donor vdisk mode on a cluster, run the following command:

```
ydb-dstool -e ydbd.endpoint cluster set --disable-donor-mode
```

### Adjust scrubbing intervals

Scrubbing is a background process that checks data integrity and performs data recovery if necessary. To disable data scrubbing on
a cluster enter the following command:

```
ydb-dstool -e ydbd.endpoint cluster set --scrub-periodicity disable
```

To set scrubbing interval to two days run the following command:

```
ydb-dstool -e ydbd.endpoint cluster set --scrub-periodicity 2d
```

### Set maximum number of simultaneously scrubbed pdisks

```
ydb-dstool -e ydbd.endpoint cluster set --max-scrubbed-disks-at-once 2
```

To above command sets maximum number if simultaneously scrubbed pdisk to two.

### Stress test failure model

To run workload that allows to stress test failure model of groups, run the following command:

```
ydb-dstool -e ydbd.endpoint cluster workload run
```

The above command performs various

* vdisk wipe
* vdisk evict
* node restart

operations until user terminates the process (e.g. by entering ```Ctrl + c```). The operations are created so that they don't
break failure model of any groups.