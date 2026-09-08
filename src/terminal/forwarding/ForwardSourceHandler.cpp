#include "ForwardSourceHandler.hpp"

namespace et {
namespace {
// On a live connection the destination response comes back within one round
// trip. Anything still unclaimed after this long means the peer is gone, and
// holding the fd both leaks it and pushes the fd numbers the select() loops
// have to cope with ever higher.
const time_t UNASSIGNED_FD_TIMEOUT_SECONDS = 300;
}  // namespace

ForwardSourceHandler::ForwardSourceHandler(
    shared_ptr<SocketHandler> _socketHandler, const SocketEndpoint& _source,
    const SocketEndpoint& _destination, bool alreadyListening)
    : socketHandler(_socketHandler),
      source(_source),
      destination(_destination) {
  if (!alreadyListening) {
    socketHandler->listen(source);
  }
}

ForwardSourceHandler::~ForwardSourceHandler() {
  socketHandler->stopListening(source);
}

int ForwardSourceHandler::listen() {
  // TODO: Replace with select
  for (int i : socketHandler->getEndpointFds(source)) {
    int fd = socketHandler->accept(i);
    if (fd > -1) {
      LOG(INFO) << "Tunnel " << source << " -> " << destination
                << " socket created with fd " << fd;
      unassignedFds[fd] = time(NULL);
      return fd;
    }
  }
  return -1;
}

void ForwardSourceHandler::update(vector<PortForwardData>* data) {
  closeExpiredUnassignedFds(time(NULL), UNASSIGNED_FD_TIMEOUT_SECONDS);

  vector<int> socketsToRemove;

  for (auto& it : socketFdMap) {
    int socketId = it.first;
    int fd = it.second;

    while (socketHandler->hasData(fd)) {
      char buf[1024];
      int bytesRead = socketHandler->read(fd, buf, 1024);
      auto readErrno = GetErrno();
      if (bytesRead == -1 &&
          (readErrno == EAGAIN || readErrno == EWOULDBLOCK)) {
        // Bail for now
        break;
      }
      PortForwardData pwd;
      pwd.set_socketid(socketId);
      pwd.set_sourcetodestination(true);
      if (bytesRead == -1) {
        VLOG(1) << "Got error reading socket " << socketId << " "
                << strerror(readErrno);
        pwd.set_error(strerror(readErrno));
      } else if (bytesRead == 0) {
        VLOG(1) << "Got close reading socket " << socketId;
        pwd.set_closed(true);
      } else {
        VLOG(1) << "Reading " << bytesRead << " bytes from socket " << socketId;
        pwd.set_buffer(string(buf, bytesRead));
      }
      data->push_back(pwd);
      if (bytesRead < 1) {
        socketHandler->close(fd);
        socketsToRemove.push_back(socketId);
        break;
      }
    }
  }
  for (auto& it : socketsToRemove) {
    socketFdMap.erase(it);
  }
}

bool ForwardSourceHandler::hasUnassignedFd(int fd) {
  return unassignedFds.find(fd) != unassignedFds.end();
}

void ForwardSourceHandler::closeUnassignedFd(int fd) {
  if (unassignedFds.find(fd) == unassignedFds.end()) {
    STERROR << "Tried to close an unassigned fd that doesn't exist";
    return;
  }
  socketHandler->close(fd);
  unassignedFds.erase(fd);
}

void ForwardSourceHandler::closeExpiredUnassignedFds(time_t now,
                                                     time_t timeoutSeconds) {
  for (auto it = unassignedFds.begin(); it != unassignedFds.end();) {
    if (now - it->second < timeoutSeconds) {
      ++it;
      continue;
    }
    LOG(WARNING) << "Closing port forward socket that was never claimed by the "
                    "peer: "
                 << it->first;
    socketHandler->close(it->first);
    it = unassignedFds.erase(it);
  }
}

void ForwardSourceHandler::addSocket(int socketId, int sourceFd) {
  if (unassignedFds.find(sourceFd) == unassignedFds.end()) {
    STERROR << "Tried to close an unassigned fd that doesn't exist "
            << sourceFd;
    return;
  }
  LOG(INFO) << "Adding socket: " << socketId << " " << sourceFd;
  unassignedFds.erase(sourceFd);
  socketFdMap[socketId] = sourceFd;
}

void ForwardSourceHandler::getActiveFds(set<int>* fds) {
  for (int fd : socketHandler->getEndpointFds(source)) {
    fds->insert(fd);
  }
  for (auto& it : socketFdMap) {
    fds->insert(it.second);
  }
  for (const auto& it : unassignedFds) {
    fds->insert(it.first);
  }
}

void ForwardSourceHandler::sendDataOnSocket(int socketId, const string& data) {
  if (socketFdMap.find(socketId) == socketFdMap.end()) {
    LOG(INFO) << "Tried to write to a socket that no longer exists!";
    return;
  }

  int fd = socketFdMap[socketId];
  const char* buf = data.c_str();
  int count = data.length();
  socketHandler->writeAllOrReturn(fd, buf, count);
}

void ForwardSourceHandler::closeSocket(int socketId) {
  auto it = socketFdMap.find(socketId);
  if (it == socketFdMap.end()) {
    LOG(WARNING) << "Tried to remove a socket that no longer exists!";
  } else {
    socketHandler->close(it->second);
    socketFdMap.erase(it);
  }
}
}  // namespace et
