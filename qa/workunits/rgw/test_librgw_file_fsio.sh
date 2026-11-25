#!/bin/sh -e
#
# FSIO's librgw suites:  write2, and nfsns with every group flag, against
# a cluster serving FSIO.  Like test_librgw_file.sh, it does no
# orchestration beyond the test user and the zonegroup's features:
# librgw's gateway is the test process, which reads the zonegroup when it
# mounts, so no gateway restarts.  Tests that check one backend's own
# state (nsfs's files, its injected failures) skip on the others.
#
# To run this test script with a cluster created via vstart.sh:
# $PATH needs to be set for radosgw-admin and ceph_test_librgw executables.
# $KEYRING needs to be set as the path for a vstart cluster's Ceph keyring.
#
# Example, with ceph cloned into $HOME and a vstart cluster running:
# $ PATH=~/ceph/build/bin/:$PATH KEYRING=~/ceph/build/keyring \
#     ~/ceph/qa/workunits/rgw/test_librgw_file_fsio.sh
# or with vstart's own user:
# $ AWS_ACCESS_KEY_ID=... AWS_SECRET_ACCESS_KEY=... LIBRGW_UID=testid ...

LIBRGW_UID=${LIBRGW_UID:-ceph-test-librgw-fsio}

if [ -z ${AWS_ACCESS_KEY_ID} ]
then
    export AWS_ACCESS_KEY_ID=`openssl rand -base64 20`
    export AWS_SECRET_ACCESS_KEY=`openssl rand -base64 40`

    radosgw-admin user create --uid ${LIBRGW_UID} \
       --access-key $AWS_ACCESS_KEY_ID \
       --secret $AWS_SECRET_ACCESS_KEY \
       --display-name "librgw fsio test user" \
       --email librgw-fsio@example.com || echo "librgw fsio user exists"
fi

# keyring override for teuthology env
if [ -n "$CEPH_KEYRING" ]; then
  KEYRING="$CEPH_KEYRING"
elif [ -z ${KEYRING} ]; then
  KEYRING="/etc/ceph/ceph.keyring"
fi
# librgw_create() takes its Ceph arguments from the last argv entry, one
# string, split there
K="-k ${KEYRING}"

# FSIO serves a bucket only where its zonegroup has the feature;  the GC
# entries FSIO sends are in manifest form.  In a realm, a period commit
# carries the change
radosgw-admin zonegroup modify --rgw-zonegroup=default --enable-feature=fsio
radosgw-admin zonegroup modify --rgw-zonegroup=default --enable-feature=gc-manifest
if radosgw-admin realm get > /dev/null 2>&1; then
    radosgw-admin period update --commit
fi

echo "fsio 1:  write2"
ceph_test_librgw_file_write2 --create --userid ${LIBRGW_UID} "${K}"

echo "fsio 2:  nfsns, every group"
ceph_test_librgw_file_nfsns --create --userid ${LIBRGW_UID} \
    --hier1 --dirs1 --marker1 --setattr --rename --readf --writef "${K}"

exit 0
