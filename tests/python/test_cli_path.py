"""`httpp path` is the only SDK helper command: install and uninstall do not exist."""

import httpp
import pytest
from httpp.__main__ import py_main as main


def test_path_without_flags_prints_all_three_labeled(capsys):
    assert main(["path"]) == 0
    out = capsys.readouterr().out.splitlines()
    assert out == [
        f"include: {httpp.get_include_dir()}",
        f"lib:     {httpp.get_lib_dir()}",
        f"cmake:   {httpp.get_cmake_dir()}",
    ]


@pytest.mark.parametrize("flag,getter", [("--include-dir", "get_include_dir"), ("--lib-dir", "get_lib_dir"), ("--cmake-dir", "get_cmake_dir")])
def test_path_flag_prints_only_the_directory(capsys, flag, getter):
    assert main(["path", flag]) == 0
    assert capsys.readouterr().out == f"{getattr(httpp, getter)()}\n"


@pytest.mark.parametrize("name", ["install", "uninstall"])
def test_install_and_uninstall_do_not_exist(name, capsys):
    with pytest.raises(SystemExit) as e:
        main([name])
    assert e.value.code == 2
    assert "invalid choice" in capsys.readouterr().err
