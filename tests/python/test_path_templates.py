"""One place parses path templates ('/users/{id}', '/users/{id:int}'): the C++ router's own parser, reached as httpp._params."""

import pytest

from httpp import Server, _params


@pytest.mark.parametrize("path, types", [
    ("/users/{id}", {"id": "str"}),
    ("/users/{id:int}", {"id": "int"}),
    ("/a/{x}/b/{y:float}/c/{z:int}", {"x": "str", "y": "float", "z": "int"}),
    ("/static/path", {}),
])
def test_names_and_types_come_from_the_core_parser(path, types):
    assert _params.path_types(path) == types
    assert _params.path_names(path) == set(types)


def test_openapi_path_drops_the_type():
    assert _params.openapi_path("/a/{x:int}/{y}/z/") == "/a/{x}/{y}/z/"
    assert _params.openapi_path("/") == "/"


@pytest.mark.parametrize("bad", [
    "/u/:id", "/u/<id>", "/u/<int:id>", "/u/{int:id}", "/u/{}", "/u/{a b}", "/u/{id", "/u/a{id}", "/u/{id:str}",
])
def test_old_forms_converters_and_junk_are_rejected_at_the_same_place(bad):
    with pytest.raises(ValueError):
        _params.path_types(bad)
    with pytest.raises(ValueError):
        Server().get(bad)(lambda: "x")  # registration goes through the same parser


def test_there_is_no_python_side_parser():
    from httpp import server

    assert not hasattr(_params, "PATH_PARAM") and not hasattr(_params, "to_cpp_pattern")
    assert not hasattr(server, "_PARAM") and not hasattr(server, "_to_pattern")
