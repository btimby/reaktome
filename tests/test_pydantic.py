import unittest

from typing import Any, Optional

from unittest.mock import MagicMock
from pydantic import BaseModel
from pydantic_collections import BaseCollectionModel

from reaktome import Reaktome, Changes, print_change


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
    def setUp(self):
        self.bar = BarModel(id='abc123', name='foo')
        Changes.on(self.bar, print_change)

    def test_reaktome_model(self):
        self.bar.name = 'bar'
        foo = self.bar.foo = FooModel(id='xyz098', name='foo')
        foo.name = 'baz'
        self.bar.foo.name = 'ben'
