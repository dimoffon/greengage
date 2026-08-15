import unittest

from gppylib.gparray import Segment, SegmentPair, ROLE_PRIMARY, ROLE_MIRROR, \
    MODE_SYNCHRONIZED, MODE_NOT_SYNC, STATUS_UP, STATUS_DOWN
from gppylib.operations.detect_unreachable_hosts import \
    mark_segments_down_for_unreachable_hosts, update_unreachable_flag_for_segments


def _segment(dbid, content, role, hostname, mode=MODE_SYNCHRONIZED):
    return Segment(content=content, preferred_role=role, dbid=dbid, role=role,
                   mode=mode, status=STATUS_UP, hostname=hostname,
                   address=hostname, port=7000 + dbid,
                   datadir='/data/seg%d' % dbid)


def _pair(primary, mirror=None):
    pair = SegmentPair()
    pair.addPrimary(primary)
    if mirror is not None:
        pair.addMirror(mirror)
    return pair


class DetectUnreachableHostsTestCase(unittest.TestCase):
    """Real gparray objects, deliberately, rather than Mocks.

    SegmentPair says of itself that "it is permissible to not have a mirror
    corresponding to a primary", and a Mock pair cannot express that: its
    mirrorDB is another auto-created Mock, so it is never None and never
    absent.  Every case below turns on the difference.
    """

    def test_marks_down_a_mirrorless_primary_on_an_unreachable_host(self):
        primary = _segment(2, 0, ROLE_PRIMARY, 'sdw1', mode=MODE_NOT_SYNC)
        mark_segments_down_for_unreachable_hosts([_pair(primary)], ['sdw1'])
        self.assertEqual(STATUS_DOWN, primary.getSegmentStatus())

    def test_leaves_a_mirrorless_primary_on_a_reachable_host_alone(self):
        primary = _segment(2, 0, ROLE_PRIMARY, 'sdw1', mode=MODE_NOT_SYNC)
        mark_segments_down_for_unreachable_hosts([_pair(primary)], ['sdw2'])
        self.assertEqual(STATUS_UP, primary.getSegmentStatus())

    def test_marks_down_both_halves_of_a_pair(self):
        primary = _segment(2, 0, ROLE_PRIMARY, 'sdw1')
        mirror = _segment(4, 0, ROLE_MIRROR, 'sdw2')
        mark_segments_down_for_unreachable_hosts([_pair(primary, mirror)],
                                                 ['sdw1', 'sdw2'])
        self.assertEqual(STATUS_DOWN, primary.getSegmentStatus())
        self.assertEqual(STATUS_DOWN, mirror.getSegmentStatus())

    def test_marks_down_only_the_half_whose_host_is_unreachable(self):
        primary = _segment(2, 0, ROLE_PRIMARY, 'sdw1')
        mirror = _segment(4, 0, ROLE_MIRROR, 'sdw2')
        mark_segments_down_for_unreachable_hosts([_pair(primary, mirror)], ['sdw2'])
        self.assertEqual(STATUS_UP, primary.getSegmentStatus())
        self.assertEqual(STATUS_DOWN, mirror.getSegmentStatus())

    def test_mixed_cluster_marks_every_segment_on_the_unreachable_host(self):
        """One content with a mirror and one without, on the same hosts.

        This is the shape that crashed: iteration reached the mirrorless pair
        and dereferenced its absent mirror, so the pair before it had already
        been handled and nothing said the run was incomplete.
        """
        mirrored_primary = _segment(2, 0, ROLE_PRIMARY, 'sdw1')
        mirrored_mirror = _segment(4, 0, ROLE_MIRROR, 'sdw2')
        lone_primary = _segment(3, 1, ROLE_PRIMARY, 'sdw1', mode=MODE_NOT_SYNC)

        mark_segments_down_for_unreachable_hosts(
            [_pair(mirrored_primary, mirrored_mirror), _pair(lone_primary)],
            ['sdw1'])

        self.assertEqual(STATUS_DOWN, mirrored_primary.getSegmentStatus())
        self.assertEqual(STATUS_UP, mirrored_mirror.getSegmentStatus())
        self.assertEqual(STATUS_DOWN, lone_primary.getSegmentStatus())

    def test_update_unreachable_flag_handles_a_mirrorless_pair(self):
        """The sibling function already guards this; keep it that way."""
        class _Array(object):
            def __init__(self, pairs):
                self.segmentPairs = pairs

        primary = _segment(2, 0, ROLE_PRIMARY, 'sdw1', mode=MODE_NOT_SYNC)
        pair = _pair(primary)
        update_unreachable_flag_for_segments(_Array([pair]), ['sdw1'])
        self.assertTrue(pair.primaryDB.unreachable)


if __name__ == '__main__':
    unittest.main()
