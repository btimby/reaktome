import unittest

from typing import Any

from unittest.mock import MagicMock
from dataclasses import dataclass

from reaktome import Reaktome, reaktiv8


@dataclass
class Foo(Reaktome):
    id: str
    name: str


class ReaktomeTestCase(unittest.TestCase):
    def test_reaktome_dataclass(self):
        foo = Foo(id='bac123', name='foo')
        foo.name = 'baz'
