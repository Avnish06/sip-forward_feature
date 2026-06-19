# Start from the official Go image
FROM golang:1.22-alpine

# Set the Current Working Directory inside the container
WORKDIR /app

# Copy the go.mod file
COPY go.mod go.sum ./

# Download all dependencies. Dependencies will be cached if the go.mod and go.sum files are not changed
# We run go mod tidy here to generate go.sum since it might not exist yet
RUN go mod tidy && go mod download

# Copy the source from the current directory to the Working Directory inside the container
COPY . .

# Build the Go app
# -o main names the binary "main"
RUN go build -o main .

# Expose port 8000 to the outside world
EXPOSE 8000

# Command to run the executable
CMD ["./main"]
