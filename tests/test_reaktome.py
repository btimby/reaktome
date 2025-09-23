import logging
import unittest

from typing import Any

from unittest.mock import MagicMock
from dataclasses import dataclass, field

from reaktome import Reaktome, on


LOGGER = logging.getLogger('reaktome')
LOGGER.addHandler(logging.StreamHandler())
LOGGER.setLevel(logging.DEBUG)


@dataclass
class TestDataClass(Reaktome):
    name: str
    price: int
    items: list[int] = field(default_factory=list)
    names: dict[str, Any] = field(default_factory=dict)


class ReaktomeTestCase(unittest.TestCase):
    def test_reaktome_singles(self):
        handler0 = MagicMock()
        handler1 = MagicMock()
        top = TestDataClass('foo', 52)
        top.on('name', handler0)
        top.on('names[foo]', handler1)
        top.name = 'bar'
        top.items.extend([[], []])
        top.items[0].append(1)
        top.names['foo'] = []
        top.names['foo'].append('bar')
        top.names['foo'].append('baz')
        handler0.assert_called_once()
        handler1.assert_called_once()

    def test_reaktome_wildcard(self):
        handler0 = MagicMock()
        top = TestDataClass('foo', 42)
        top.on('*', handler0)
        top.name = 'bar'
        handler0.assert_called_once_with('name', 'foo', 'bar')
