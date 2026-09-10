"""Basic tests for httpp."""

import pytest


def test_hello_world():
    """Test the hello_world function."""
    from httpp import hello_world

    result = hello_world()
    assert result == "Hello, World from httpp!"


def test_calculator_class():
    """Test Calculator class functionality."""
    from httpp.httpp import Calculator

    # Test basic functionality
    calc = Calculator()
    assert calc.get_value() == 0.0

    # Test addition
    calc.add(5.0)
    assert calc.get_value() == 5.0

    # Test subtraction
    calc.subtract(2.0)
    assert calc.get_value() == 3.0

    # Test multiplication
    calc.multiply(3.0)
    assert calc.get_value() == 9.0

    # Test division
    calc.divide(3.0)
    assert calc.get_value() == 3.0

    # Test with initial value
    calc2 = Calculator(10.0)
    assert calc2.get_value() == 10.0


def test_calculator_divide_by_zero():
    """Test Calculator division by zero handling."""
    from httpp.httpp import Calculator

    calc = Calculator(10.0)
    with pytest.raises(ZeroDivisionError):
        calc.divide(0.0)


def test_calculator_reset():
    """Test Calculator reset functionality."""
    from httpp.httpp import Calculator

    calc = Calculator(5.0)
    assert calc.get_value() == 5.0

    calc.reset()
    assert calc.get_value() == 0.0


if __name__ == "__main__":
    pytest.main([__file__, "-v"])