"""One place parses path templates ('/users/{id}', '/users/<id>', '/users/:id'): httpp._params."""

import pytest

from httpp import Server, _params


@pytest.mark.parametrize("path, cpp, names", [
    ("/users/{id}", "/users/:id", {"id"}),
    ("/users/<id>", "/users/:id", {"id"}),
    ("/users/:id", "/users/:id", {"id"}),
    ("/a/{x}/b/<y>/c/:z", "/a/:x/b/:y/c/:z", {"x", "y", "z"}),
    ("/static/path", "/static/path", set()),
])
def test_one_parser_for_cpp_pattern_names_and_openapi(path, cpp, names):
    assert _params.to_cpp_pattern(path) == cpp
    assert _params.path_names(path) == names


def test_openapi_path_uses_braces():
    assert _params.openapi_path("/a/<x>/:y/{z}") == "/a/{x}/{y}/{z}"


@pytest.mark.parametrize("bad", ["/u/{int:id}", "/u/<int:id>", "/u/{}", "/u/{a b}"])
def test_converters_and_junk_are_rejected_at_the_same_place(bad):
    with pytest.raises(ValueError):
        _params.to_cpp_pattern(bad)
    with pytest.raises(ValueError):
        Server().get(bad)(lambda: "x")  # registration goes through the same function


def test_the_server_module_has_no_parser_of_its_own():
    from httpp import server

    assert not hasattr(server, "_PARAM") and not hasattr(server, "_to_pattern")
