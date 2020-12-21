#!/bin/bash

set -euo pipefail

echo ":docker::package: Push EOS ubuntu 18.04 base Image to Dockerhub"

# buildkite-agent artifact download someversion.deb .

echo ":download: Downloading artifact from Buildkite step Ubuntu 18.04"
buildkite-agent artifact download '*.deb' --step ':ubuntu: Ubuntu 18.04 - Package Builder' .
echo ":done: download successfull"

PREFIX='base-ubuntu-18.04'
SANITIZED_BRANCH=$(echo "$BUILDKITE_BRANCH" | sed 's.^/..' | sed 's/[:/]/_/g')
SANITIZED_TAG=$(echo "$BUILDKITE_TAG" | sed 's.^/..' | tr '/' '_')
echo "$SANITIZED_BRANCH"
echo "$SANITIZED_TAG"

# do docker build
echo ":docker::build: Building image..."

set -e
IMAGE="$EOSIO_REGISTRY:eosio-ubuntu-18.04-$BUILDKITE_COMMIT-bin"

echo ":docker: Building Image...."

docker build -t $IMAGE -f ./docker/dockerfile .
echo "Build Done successfully!!"

#do docker push to ECR
echo "Pushing Image to ECR..."

docker push $IMAGE
echo ":done: Done"

# do docker push Dockerhub
echo "Tag Images to be pushed to Dockerhub EOSIO/EOS...."
DOCKERHUB_EOS_REGISTRY="/eosio/eos"
docker tag ${IMAGE} ${DOCKERHUB_EOS_REGISTRY}/${PREFIX}:${SANITIZED_BRANCH}
echo "done.. image tagged"
docker images

echo "Pushing Image DockerHub EOSIO/EOS ..."
docker push $DOCKERHUB_EOS_REGISTRY/${PREFIX}:${SANITIZED_BRANCH}   
echo ":done: Done"

#do clean up image
echo "Cleaning up ${IMAGE}"
docker rmi $IMAGE
echo ":clean: cleaned"
