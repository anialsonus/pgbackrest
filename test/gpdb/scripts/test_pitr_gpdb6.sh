#!/usr/bin/env bash
set -exo pipefail

PGBACKREST_TEST_DIR=/home/gpadmin/test_pgbackrest
PGBACKREST_BIN=/usr/local/bin
GPHOME=/usr/local/greenplum-db-devel

echo "Starting up demo cluster" > /dev/null
source $GPHOME/greenplum_path.sh
pushd gpdb_src/gpAux/gpdemo
make create-demo-cluster WITH_MIRRORS=true
source gpdemo-env.sh
popd


echo "Creating backup and log directories for pgbackrest..." > /dev/null
TEST_NAME=$(basename "${0%.sh}")
mkdir -p "$PGBACKREST_TEST_DIR/logs/$TEST_NAME"
mkdir -p "$PGBACKREST_TEST_DIR/$TEST_NAME"

DATADIR="${MASTER_DATA_DIRECTORY%*/*/*}"
MASTER=${DATADIR}/qddir/demoDataDir-1
PRIMARY1=${DATADIR}/dbfast1/demoDataDir0
PRIMARY2=${DATADIR}/dbfast2/demoDataDir1
PRIMARY3=${DATADIR}/dbfast3/demoDataDir2
MIRROR1=${DATADIR}/dbfast_mirror1/demoDataDir0
MIRROR2=${DATADIR}/dbfast_mirror2/demoDataDir1
MIRROR3=${DATADIR}/dbfast_mirror3/demoDataDir2
MASTER_PORT=6000
PRIMARY1_PORT=6002
PRIMARY2_PORT=6003
PRIMARY3_PORT=6004

echo "Filling the pgbackrest.conf configuration file" > /dev/null
cat <<EOF > $PGBACKREST_TEST_DIR/pgbackrest.conf
[seg-1]
pg1-path=$MASTER
pg1-port=$MASTER_PORT

[seg0]
pg1-path=$PRIMARY1
pg1-port=$PRIMARY1_PORT

[seg1]
pg1-path=$PRIMARY2
pg1-port=$PRIMARY2_PORT

[seg2]
pg1-path=$PRIMARY3
pg1-port=$PRIMARY3_PORT

[global]
repo1-path=$PGBACKREST_TEST_DIR/$TEST_NAME
log-path=$PGBACKREST_TEST_DIR/logs/$TEST_NAME
start-fast=y
fork=GPDB
EOF

echo "Initiallizing pgbackrest for GPDB" > /dev/null
for i in -1 0 1 2
do 
    PGOPTIONS="-c gp_session_role=utility" $PGBACKREST_BIN/pgbackrest \
    --config $PGBACKREST_TEST_DIR/pgbackrest.conf --stanza=seg$i stanza-create
done

echo "Configuring WAL archiving command" > /dev/null
gpconfig -c archive_mode -v on

gpconfig -c archive_command -v "'PGOPTIONS=\"-c gp_session_role=utility\" \
$PGBACKREST_BIN/pgbackrest --config $PGBACKREST_TEST_DIR/pgbackrest.conf \
--stanza=seg%c archive-push %p'" --skipvalidation

gpstop -ar

echo "pgbackrest health check" > /dev/null
for i in -1 0 1 2
do 
	PGOPTIONS="-c gp_session_role=utility" $PGBACKREST_BIN/pgbackrest \
    --config $PGBACKREST_TEST_DIR/pgbackrest.conf --stanza=seg$i check
done

# The test scenario starts here
echo "Creating initial dataset..." > /dev/null
createdb gpdb_pitr_database
psql -d gpdb_pitr_database -c \
"CREATE TABLE t1 (id int, text varchar(255)) DISTRIBUTED BY (id);"

psql -d gpdb_pitr_database -c \
"INSERT INTO t1 SELECT i, 'text'||i FROM generate_series(1,30) i;"

psql -d gpdb_pitr_database -c "SELECT * FROM t1 ORDER BY id;" \
-o $PGBACKREST_TEST_DIR/$TEST_NAME/t1_rows_original.out

echo "Creating full backup on master and seg0..." > /dev/null
for i in -1 0
do 
    PGOPTIONS="-c gp_session_role=utility" $PGBACKREST_BIN/pgbackrest \
    --config $PGBACKREST_TEST_DIR/pgbackrest.conf --stanza=seg$i \
    --type=full backup
done

echo "Checking the presence of first backup" > /dev/null
function check_backup(){
    segment_backup_dir=$PGBACKREST_TEST_DIR/$TEST_NAME/backup/seg$1
    current_date=$(date +%Y%m%d)
    if [[ $(find $segment_backup_dir -maxdepth 1 -type d -name \
        "${current_date}-??????F" -not -empty ) ]];
    then
        echo "Found a backup directory for segment $1: $dirname" > /dev/null
    else
        echo "The backup directory for segment $1 was not found" > /dev/null
        exit 1
    fi
}
for i in -1 0
do 
   check_backup $i
done

echo "Creating additional table..." > /dev/null
psql -d gpdb_pitr_database -c \
"CREATE TABLE t2 (id int, text varchar(255)) DISTRIBUTED BY (id);"

psql -d gpdb_pitr_database -c \
"INSERT INTO t2 SELECT i, 'text'||i FROM generate_series(1,30) i;"

psql -d gpdb_pitr_database -c "SELECT * FROM t2 ORDER BY id;" \
-o $PGBACKREST_TEST_DIR/$TEST_NAME/t2_rows_original.out

echo "Creating full backup on seg1 and seg2..." > /dev/null
for i in 1 2
do 
    PGOPTIONS="-c gp_session_role=utility" $PGBACKREST_BIN/pgbackrest \
    --config $PGBACKREST_TEST_DIR/pgbackrest.conf --stanza=seg$i \
    --type=full backup
done

echo "Checking the presence of second backup" > /dev/null
for i in 1 2
do 
   check_backup $i
done

echo "Creating a distributed restore point" > /dev/null
psql -d gpdb_pitr_database -c "create extension gp_pitr;"
psql -d gpdb_pitr_database -c "select gp_create_restore_point('test_pitr');"
psql -d gpdb_pitr_database -c "select gp_switch_wal();"

echo "Simulating disaster..." > /dev/null
psql -d gpdb_pitr_database -c "drop table t1;"
psql -d gpdb_pitr_database -c "truncate table t2;"

gpstop -a
rm -rf $MASTER/* $PRIMARY1/* $PRIMARY2/* $PRIMARY3/*
rm -rf $MIRROR1/* $MIRROR2/* $MIRROR3/* $DATADIR/standby/*

echo "Restoring cluster..." > /dev/null
for i in -1 0 1 2
do 
	PGOPTIONS="-c gp_session_role=utility" $PGBACKREST_BIN/pgbackrest --config \
    $PGBACKREST_TEST_DIR/pgbackrest.conf --stanza=seg$i --type=name \
    --target=test_pitr restore
done

echo "Configure mirrors after primary restore" > /dev/null
gpstart -am
gpinitstandby -ar

PGOPTIONS="-c gp_session_role=utility" psql -d gpdb_pitr_database << EOF
SET allow_system_table_mods to true;
UPDATE gp_segment_configuration
SET status = CASE WHEN role='m' THEN 'd' ELSE status END, mode = 'n'
WHERE content >= 0;
EOF

gpstop -ar
gprecoverseg -aF
gpinitstandby -as $HOSTNAME -S $DATADIR/standby/ -P 6001

echo "Checking data integrity" > /dev/null
psql -d gpdb_pitr_database -c "SELECT * FROM t1 ORDER BY id;" \
-o $PGBACKREST_TEST_DIR/$TEST_NAME/t1_rows_restored.out

psql -d gpdb_pitr_database -c "SELECT * FROM t2 ORDER BY id;" \
-o $PGBACKREST_TEST_DIR/$TEST_NAME/t2_rows_restored.out

if diff "$PGBACKREST_TEST_DIR/$TEST_NAME/t1_rows_original.out" \
"$PGBACKREST_TEST_DIR/$TEST_NAME/t1_rows_restored.out"; then
    echo "Rows match." > /dev/null
else
    echo "Discrepancy in rows found." > /dev/null
    exit 1
fi

if diff "$PGBACKREST_TEST_DIR/$TEST_NAME/t2_rows_original.out" \
"$PGBACKREST_TEST_DIR/$TEST_NAME/t2_rows_restored.out"; then
    echo "Rows match." > /dev/null
else
    echo "Discrepancy in rows found." > /dev/null
    exit 1
fi