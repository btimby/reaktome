import unittest

from typing import Any

from unittest.mock import MagicMock
from dataclasses import dataclass

from reaktome import Reaktome, ReaktomeWatch, reaktiv8


@dataclass
class Foo(Reaktome):
    id: str
    name: str


class ReaktomeTestCase(unittest.TestCase):
    def test_reaktome_dataclass(self):
        handler = MagicMock()
        obj = Foo(id='abc123', name='Roger')
        obj.on('*', handler)
        obj.name = 'Steve'
        handler.assert_called_once()
