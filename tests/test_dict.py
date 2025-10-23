import unittest

from copy import deepcopy

from reaktome import reaktiv8, Changes


class ReaktomeDictTestCase(unittest.TestCase):
    def setUp(self):
        self.d = {}
        self.changes = []
        reaktiv8(self.d)
        Changes.on(self.d, self.changes.append)

    def test_setitem_triggers_hook(self):
        self.d["a"] = 1
        self.assertEqual(len(self.changes), 1)

    def test_update_triggers_hook(self):
        self.d.update({'a': 1})
        self.assertEqual(len(self.changes), 1)

    def test_update_kwargs_triggers_hook(self):
        self.d.update(a=1)
        self.assertEqual(len(self.changes), 1)

    def test_update_triggers_hooks(self):
        self.d.update({"x": 10, "y": 20})
        # expect 2 hooks, one per inserted key
        self.assertEqual(len(self.changes), 2)

    def test_setitem_overwrites_and_triggers_hook(self):
        self.d["a"] = 1
        self.changes.clear()
        self.d["a"] = 2
        # overwrite should still trigger a change
        self.assertEqual(len(self.changes), 1)

    def test_delitem_triggers_hook(self):
        self.d["k"] = "v"
        self.changes.clear()
        del self.d["k"]
        self.assertEqual(len(self.changes), 1)

    def test_delitem_missing_raises_and_no_hook(self):
        with self.assertRaises(KeyError):
            del self.d["missing"]
        self.assertEqual(len(self.changes), 0)

    def test_clear_triggers_hooks(self):
        self.d.update({"a": 1, "b": 2, "c": 3})
        self.changes.clear()
        self.d.clear()
        # expect 3 hooks, one per removed key
        self.assertEqual(len(self.changes), 3)

    def test_deepcopy(self):
        deepcopy(self.d)
