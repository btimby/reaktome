import unittest

from typing import Any, Optional

from unittest.mock import MagicMock
from pydantic import BaseModel
from pydantic_collections import BaseCollectionModel

from reaktome import Reaktome, reaktiv8


class FooModel(Reaktome, BaseModel):
    id: str
    name: str


class BarModel(Reaktome, BaseModel):
    id: str
    name: str
    foo: Optional[FooModel] = None


class FooModelCollection(Reaktome, BaseCollectionModel[FooModel]):
    pass


class BarModelCollection(Reaktome, BaseCollectionModel[BarModel]):
    pass


class ReaktomeTestCase(unittest.TestCase):
    def test_reaktome_model(self):
        bar = BarModel(id='abc123', name='foo')
        bar.name = 'bar'
        foo = bar.foo = FooModel(id='xyz098', name='foo')
        foo.name = 'baz'
        bar.foo.name = 'ben'
