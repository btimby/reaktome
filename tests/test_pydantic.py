import unittest

from typing import Any, Optional

from unittest.mock import MagicMock
from pydantic import BaseModel
from pydantic_collections import BaseCollectionModel

from reaktome import Reaktome, Changes


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
        self.changes = []
        Changes.on(self.bar, self.changes.append)

    def test_reaktome_model(self):
        self.bar.name = 'bar'
        foo = self.bar.foo = FooModel(id='xyz098', name='foo')
        foo.name = 'baz'
        self.bar.foo.name = 'ben'

    def test_model_dump(self):
        self.assertEqual(
            {'foo': None, 'id': 'abc123', 'name': 'foo'},
            self.bar.model_dump(),
        )


class ReaktomeCollectionTestCase(unittest.TestCase):
    def setUp(self):
        self.bar_coll = BarModelCollection()
        self.changes = []
        Changes.on(self.bar_coll, self.changes.append)

    def test_append(self):
        self.bar_coll.append(BarModel(id='abc123', name='foo'))
        self.assertEqual(len(self.changes), 1)