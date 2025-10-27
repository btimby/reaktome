from unittest import TestCase

from pydantic import BaseModel
from pydantic_collections import BaseCollectionModel

from reaktome import Reaktome, Changes


class Player(Reaktome, BaseModel):
    name: str


class Team(Reaktome, BaseCollectionModel[Player]):
    pass


class Game(Reaktome, BaseModel):
    team1: Team = Team()
    team2: Team = Team()


class CollectionTestCase(TestCase):
    def setUp(self) -> None:
        self.changes = []
        self.team = Team()
        Changes.on(self.team, self.changes.append)

    def test_team_append(self) -> None:
        self.team.append(Player(name='Ben'))
        self.team.append(Player(name='Tom'))
        self.assertEqual(2, len(self.changes))


def handler(l):
    def inner(change):
        l.append(change)
    return inner


class NestedCollectionTestCase(TestCase):
    def setUp(self) -> None:
        self.changes = []
        self.game = Game()
        Changes.on(self.game, handler(self.changes))

    def test_game_team_append(self) -> None:
        self.game.team1.append(Player(name='Ben'))
        print('CHANGES:', self.changes)
        self.assertEqual(1, len(self.changes))
