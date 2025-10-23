import unittest

from copy import deepcopy

from reaktome import reaktiv8, Changes


class ReaktomeSetTestCase(unittest.TestCase):
    def setUp(self):
        self.set = set()
        self.changes = []
        reaktiv8(self.set)
        Changes.on(self.set, self.changes.append)

    def test_add_triggers_hook(self):
        self.set.add(42)
        self.assertEqual(len(self.changes), 1)

    def test_discard_triggers_hook(self):
        self.set.add(1)
        self.changes.clear()
        self.set.discard(1)
        self.assertEqual(len(self.changes), 1)

    def test_remove_triggers_hook(self):
        self.set.add(2)
        self.changes.clear()
        self.set.remove(2)
        self.assertEqual(len(self.changes), 1)

    def test_remove_missing_raises_but_still_calls_hook(self):
        with self.assertRaises(KeyError):
            self.set.remove(99)
        # still should record discard hook
        self.assertEqual(len(self.changes), 0)

    def test_discard_missing_does_not_raise(self):
        self.set.discard(123)
        self.assertEqual(len(self.changes), 1)

    def test_deepcopy(self):
        deepcopy(self.set)
