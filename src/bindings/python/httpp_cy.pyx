# distutils: language = c++
# cython: language_level=3

"""
A Cython project with scikit-build
"""

from libc.stdlib cimport malloc, free
from libc.string cimport strcpy, strlen
from libcpp.string cimport string
from libcpp cimport bool


def hello_world():
    """A simple function that returns a greeting."""
    return "Hello, World from httpp!"


cdef class Calculator:
    """A simple calculator class implemented in Cython."""
    
    cdef double value
    
    def __init__(self, initial_value=0.0):
        self.value = initial_value
    
    def add(self, double x):
        """Add a value to the calculator's internal value."""
        self.value += x
        return self.value
    
    def subtract(self, double x):
        """Subtract a value from the calculator's internal value."""
        self.value -= x
        return self.value
    
    def multiply(self, double x):
        """Multiply the calculator's internal value by x."""
        self.value *= x
        return self.value
    
    def divide(self, double x):
        """Divide the calculator's internal value by x."""
        if x == 0:
            raise ZeroDivisionError("Cannot divide by zero")
        self.value /= x
        return self.value
    
    def get_value(self):
        """Get the current value."""
        return self.value
    
    def reset(self):
        """Reset the calculator to zero."""
        self.value = 0.0
        return self.value