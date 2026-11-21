from typing import List, Optional

from httpp import HTTPException, Query, Server
from httpp.models import BaseModel, Field

app = Server(
    title="Library API",
    version="1.2.0",
    description="A small API for a library: list, read, add and remove books.",
)


class BookIn(BaseModel):
    title: str = Field(min_length=1, max_length=120, description="The title of the book.", examples=["The Pragmatic Programmer"])
    author: str = Field(min_length=1, description="Who wrote it.", examples=["Andrew Hunt"])
    year: int = Field(ge=1450, le=2100, description="The year it was first published.", examples=[1999])
    tags: List[str] = Field(default_factory=list, description="Free words to find it by.", examples=[["software", "craft"]])


class Book(BookIn):
    id: int


BOOKS = {
    1: Book(id=1, title="The Pragmatic Programmer", author="Andrew Hunt", year=1999, tags=["software", "craft"]),
    2: Book(id=2, title="Structure and Interpretation of Computer Programs", author="Harold Abelson", year=1985, tags=["software", "lisp"]),
    3: Book(id=3, title="A Philosophy of Software Design", author="John Ousterhout", year=2018, tags=["software", "design"]),
}


@app.get("/books", tags=["books"], summary="List books", response_model=List[Book])
def list_books(
    q: Optional[str] = Query(None, description="Only books whose title contains this text."),
    limit: int = Query(10, ge=1, le=100, description="How many books to return at most."),
):
    """The books in the library, oldest id first."""
    found = [b for b in BOOKS.values() if q is None or q.lower() in b.title.lower()]
    return found[:limit]


@app.get("/books/{book_id}", tags=["books"], summary="Read one book", response_model=Book)
def read_book(book_id: int):
    """One book by its id. Answers 404 when there is none."""
    if book_id not in BOOKS:
        raise HTTPException(404, "no such book")
    return BOOKS[book_id]


@app.post("/books", tags=["books"], summary="Add a book", response_model=Book, status_code=201)
def add_book(book: BookIn):
    """Add a book. The id is chosen by the library."""
    new_id = max(BOOKS) + 1
    BOOKS[new_id] = Book(id=new_id, **book.model_dump())
    return BOOKS[new_id]


@app.delete("/books/{book_id}", tags=["books"], summary="Remove a book", status_code=204)
def remove_book(book_id: int):
    if BOOKS.pop(book_id, None) is None:
        raise HTTPException(404, "no such book")


@app.get("/books/by-author/{author}", tags=["books"], summary="Find books by an author", deprecated=True)
def by_author(author: str):
    """Use `GET /books?q=` instead."""
    return [b for b in BOOKS.values() if author.lower() in b.author.lower()]


@app.get("/health", tags=["service"], summary="Is the library up?")
def health():
    return {"status": "ok"}


if __name__ == "__main__":
    app.listen("127.0.0.1", 8000)
