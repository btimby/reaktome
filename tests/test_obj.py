import unittest

from copy import deepcopy

from reaktome import reaktiv8, Changes


class Foo:
    def __init__(self, id: str, name: str):
        self.id = id
        self.name = name


class ReaktomeObjTestCase(unittest.TestCase):
    def setUp(self):
        self.obj = Foo("x", "y")
        reaktiv8(self.obj)
        self.changes = []
        Changes.on(self.obj, self.changes.append)

    def test_setattr_triggers_hook(self):
        self.obj.name = "z"
        self.assertEqual(self.obj.name, "z")
        self.assertTrue(any(c.source == "attr" for c in self.changes))

    def test_delattr_triggers_hook(self):
        del self.obj.name
        self.assertFalse(hasattr(self.obj, "name"))
        self.assertTrue(any(c.source == "attr" for c in self.changes))

    def test_deepcopy(self):
        deepcopy(self.obj)
