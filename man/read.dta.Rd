\name{read.dta}
\alias{read.dta}
%- Also NEED an `\alias' for EACH other topic documented here.
\title{Read Stata binary files}
\usage{
read.dta(filename)
}
%- maybe also `usage' for other objects documented here.
\arguments{
 \item{filename}{a filename as a character string}
}
\description{
Reads a file in Stata v6.0 binary format into a dataframe.  This
function currently cannot convert between machines with different byte
orderings (basiclly Intel chips vs everything else)
}
\details{
The variables in the Stata data set become the columns of the data
frame. Missing values are correctly handled. The data label, variable labels, and
timestamp are stored as attributes of the data frame. Nothing is done
with variable characteristics, print formats, or value labels.
}
\value{
  a data frame
}
\references{Stata v6.0 Users Manual describes the format of the files}
\author{Thomas Lumley}


\seealso{\code{\link{write.dta}},\code{\link{attributes}}}

\examples{
data(swiss)
write.dta(swiss,swissfile<-tempfile())
read.dta(swissfile)
}
\keyword{file}%-- one or more ...
