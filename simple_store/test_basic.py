#!/usr/bin/env python2
import argparse
from gotthard import *

parser = argparse.ArgumentParser()
parser.add_argument("--log", "-l", type=str, help="filename to write log to", default=None)
parser.add_argument("host", type=str, help="server hostname")
parser.add_argument("port", type=int, help="server port")
args = parser.parse_args()

r, w = GotthardClient.r, GotthardClient.w

with GotthardClient(store_addr=(args.host, args.port), log_filename=args.log) as cl:

    # Check that we can write multilpe values
    for i in range(3):
        cl.req_id_seq += 1
        req_id = cl.req_id_seq
        value = str(unichr(97+i))
        res = cl.req(w(i+1, value), req_id=req_id)
        assert(res.flags.type == TYPE_RES)
        assert(res.status == STATUS_OK)
        assert(res.cl_id == cl.cl_id)
        assert(res.req_id == req_id)
        assert(len(res.ops) == 1)
        assert(res.ops[0].key == i+1)
        assert(res.ops[0].type == TXN_UPDATED)
        assert(res.ops[0].value[0] == value)

    # Check that we can read those values
    for i in range(3):
        cl.req_id_seq += 1
        req_id = cl.req_id_seq
        value = str(unichr(97+i))
        res = cl.req(r(i+1), req_id=req_id)
        assert(res.flags.type == TYPE_RES)
        assert(res.status == STATUS_OK)
        assert(res.cl_id == cl.cl_id)
        assert(res.req_id == req_id)
        assert(len(res.ops) == 1)
        assert(res.ops[0].key == i+1)
        assert(res.ops[0].type == TXN_VALUE)
        assert(res.ops[0].value[0] == value)

    # Try reading multiple values
    res = cl.req([r(1), r(2)])
    assert(res.status == STATUS_OK)
    assert(len(res.ops) == 2)
    assert(res.op(k=1).key == 1)
    assert(res.op(k=1).type == TXN_VALUE)
    assert(res.op(k=1).value.rstrip('\0') == 'a')
    assert(res.op(k=2).key == 2)
    assert(res.op(k=2).type == TXN_VALUE)
    assert(res.op(k=2).value.rstrip('\0') == 'b')

    # Try writing multiple values
    res = cl.req([w(4, 'hello'), w(5, 'world')])
    assert(res.status == STATUS_OK)
    assert(len(res.ops) == 2)
    assert(res.op(k=4).type == TXN_UPDATED)
    assert(res.op(k=4).value.rstrip('\0') == 'hello')
    assert(res.op(k=5).type == TXN_UPDATED)
    assert(res.op(k=5).value.rstrip('\0') == 'world')

    # Try a good r/w
    res1 = cl.req(r(1))
    assert(res1.status == STATUS_OK)
    res2 = cl.req([r(1, res1.ops[0].value), w(1, 'x')])
    assert(len(res2.ops) == 1)
    assert(res2.ops[0].type == TXN_UPDATED)
    assert(res2.ops[0].value[0] == 'x')

    # Try a bad r/w
    res = cl.req([r(1, 'notthesame'), w(1, 'x')])
    assert(res.status == STATUS_ABORT)
    assert(len(res.ops) == 1)
    assert(res.ops[0].type == TXN_VALUE)
    assert(res.ops[0].value == res2.ops[0].value)

    # Try writing multiple values
    res = cl.req([w(1, 'a'), w(2, 'b')])
    assert(res.status == STATUS_OK)
    assert(len(res.ops) == 2)
    assert(res.op(k=1).key == 1)
    assert(res.op(k=1).type == TXN_UPDATED)
    assert(res.op(k=1).value.rstrip('\0') == 'a')
    assert(res.op(k=2).key == 2)
    assert(res.op(k=2).type == TXN_UPDATED)
    assert(res.op(k=2).value.rstrip('\0') == 'b')

    # Try a good RW with multiple reads
    res = cl.req([r(1, 'a'), r(2, 'b'), w(3, 'zzz')])
    assert(res.status == STATUS_OK)
    assert(len(res.ops) == 1)
    assert(res.ops[0].key == 3)
    assert(res.ops[0].type == TXN_UPDATED)
    assert(res.ops[0].value.rstrip('\0') == 'zzz')

    # Try a good RW with multiple writes
    res = cl.req([r(1, 'a'), r(2, 'b'), w(3, 'c'), w(4, 'd')])
    assert(res.status == STATUS_OK)
    assert(len(res.ops) == 2)
    assert(res.op(k=3).key == 3)
    assert(res.op(k=3).type == TXN_UPDATED)
    assert(res.op(k=3).value.rstrip('\0') == 'c')
    assert(res.op(k=4).key == 4)
    assert(res.op(k=4).type == TXN_UPDATED)
    assert(res.op(k=4).value.rstrip('\0') == 'd')

    # Try a bad RW with multiple reads
    res = cl.req([r(1, 'a'), r(2, 'wrong'), w(3, 'x')])
    assert(res.status == STATUS_ABORT)
    assert(len(res.ops) > 0)
    assert(res.op(k=2).type == TXN_VALUE) # should contain correct value
    assert(res.op(k=2).value.rstrip('\0') == 'b')

    # Try a bad RW with multiple bad reads
    res = cl.req([r(1, 'wrong'), r(2, 'wrong'), w(3, 'x')])
    assert(res.status == STATUS_ABORT)
    assert(len(res.ops) > 1)
    assert(res.op(k=1).type == TXN_VALUE) # should contain correct value
    assert(res.op(k=1).value.rstrip('\0') == 'a')
    assert(res.op(k=2).type == TXN_VALUE) # should contain correct value
    assert(res.op(k=2).value.rstrip('\0') == 'b')

    # Send some null operations
    res = cl.req([TxnOp(t=TXN_NOP, key=0), TxnOp(t=TXN_NOP, key=0)])
    assert(res.status == STATUS_BADREQ)

    # Access a 20-bit key
    res = cl.req(w(2**20, '20bits'))
    assert(res.status == STATUS_OK)
    res = cl.req(r(2**20))
    assert(res.op(k=2**20).value.rstrip('\0') == '20bits')
    assert(cl.req(w(2**20, '')).status == STATUS_OK)

    # Cleanup: write null to the keys
    for i in range(5):
        res1 = cl.req(w(i+1, ''))
        assert(res1.status == STATUS_OK)
        assert(res1.ops[0].key == i+1)
        assert(res1.ops[0].value.rstrip('\0') == '')
        res2 = cl.req(r(i+1))
        assert(res2.status == STATUS_OK)
        assert(res2.ops[0].value.rstrip('\0') == '')

    cl.close()
