import unittest

from copy import deepcopy
from typing import Any, Optional

from unittest.mock import MagicMock
from pydantic import BaseModel
from pydantic_collections import BaseCollectionModel

from reaktome import Reaktome, reaktiv8, Changes


class Foo:
    def __init__(self, id: str, name: str):
        self.id = id
        self.name = name


class FooList(Reaktome, list):
    pass


class ReaktomeListTestCase(unittest.TestCase):
    def setUp(self):
        self.list = list()
        reaktiv8(self.list)
        self.changes = []
        Changes.on(self.list, self.changes.append)

    def test_append(self):
        self.list.append("a")
        self.assertEqual(self.list, ["a"])
        self.assertEqual(len(self.changes), 1)
        self.assertEqual(self.changes[-1].source, "item")

    def test_extend(self):
        self.list.extend(["a", "b"])
        self.assertEqual(self.list, ["a", "b"])
        self.assertEqual(len(self.changes), 2)  # one event per element

    def test_insert(self):
        self.list.extend([1, 3])
        self.list.insert(1, 2)
        self.assertEqual(self.list, [1, 2, 3])
        self.assertEqual(len(self.changes), 3)
        self.assertEqual(self.changes[-1].source, "item")

    def test_remove(self):
        self.list.extend([1, 2, 3])
        self.list.remove(2)
        self.assertEqual(self.list, [1, 3])
        # remove should trigger a delitem
        self.assertTrue(any(c.source == "item" for c in self.changes))

    def test_pop(self):
        self.list.extend([1, 2, 3])
        val = self.list.pop(1)
        self.assertEqual(val, 2)
        self.assertEqual(self.list, [1, 3])
        self.assertTrue(any(c.source == "item" for c in self.changes))

    def test_clear(self):
        self.list.extend([1, 2, 3])
        self.list.clear()
        self.assertEqual(self.list, [])
        # clear should fire delitem for each element
        del_events = [c for c in self.changes if c.source == "item"]
        self.assertGreaterEqual(len(del_events), 3)

    def test_deepcopy(self):
        deepcopy(self.list)
