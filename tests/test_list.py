import unittest

from typing import Any, Optional

from unittest.mock import MagicMock
from pydantic import BaseModel
from pydantic_collections import BaseCollectionModel

from reaktome import Reaktome, reaktiv8, Changes, print_change


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
        Changes.on(self.list, print_change)

    def test_pop(self):
        self.list.append(1)
        self.assertEqual(1, self.list.pop())

class ReaktomeChangeTestCase(unittest.TestCase):
    def test_reaktome_obj(self):
        foo = FooList()
        foo.append(Foo('abc123', 'foo'))

