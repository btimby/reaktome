import unittest

from copy import deepcopy
from typing import Any

from unittest.mock import MagicMock
from dataclasses import dataclass

from reaktome import Reaktome, reaktiv8


@dataclass
class Foo(Reaktome):
    id: str
    name: str


class ReaktomeTestCase(unittest.TestCase):
    def setUp(self):
        self.foo = Foo(id='bac123', name='foo')

    def test_reaktome_dataclass(self):
        self.foo.name = 'baz'

    def test_deepcopy(self):
        deepcopy(self.foo)
