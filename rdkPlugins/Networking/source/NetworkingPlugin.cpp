/*
* If not stated otherwise in this file or this component's LICENSE file the
* following copyright and licenses apply:
*
* Copyright 2020 Sky UK
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "NetworkingPlugin.h"
#include "DnsmasqSetup.h"
#include "PortForwarding.h"
#include "MulticastForwarder.h"
#include "NetworkSetup.h"
#include "Netlink.h"

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

REGISTER_RDK_PLUGIN(NetworkingPlugin);

NetworkingPlugin::NetworkingPlugin(std::shared_ptr<rt_dobby_schema> &cfg,
                                   const std::shared_ptr<DobbyRdkPluginUtils> &utils,
                                   const std::string &rootfsPath)
    : mName("Networking"),
      mNetworkType(NetworkType::None),
      mContainerConfig(cfg),
      mUtils(utils),
      mRootfsPath(rootfsPath),
      mIpcService(nullptr),
      mDobbyProxy(nullptr),
      mNetfilter(std::make_shared<Netfilter>())
{
    AI_LOG_FN_ENTRY();

    if (!mContainerConfig || !cfg->rdk_plugins->networking ||
        !cfg->rdk_plugins->networking->data)
    {
        mValid = false;
    }
    else
    {
        mPluginData = cfg->rdk_plugins->networking->data;
        mHelper = std::make_shared<NetworkingHelper>(mPluginData->ipv4, mPluginData->ipv6);

        std::string networkType = mPluginData->type;
        if (networkType == "nat")
        {
            mNetworkType = NetworkType::Nat;
        }
        else if (networkType == "none")
        {
            mNetworkType = NetworkType::None;
        }
        else if (networkType == "open")
        {
            mNetworkType = NetworkType::Open;
        }
        else
        {
            AI_LOG_WARN("Unexpected network type '%s', defaulting to 'none'",
                        networkType.c_str());
            mNetworkType = NetworkType::None;
        }
        mValid = true;
    }

    AI_LOG_FN_EXIT();
}

NetworkingPlugin::~NetworkingPlugin()
{
    AI_LOG_FN_ENTRY();

    // stop the remote service if it was initialised
    if (mIpcService)
    {
        mIpcService->stop();
    }

    AI_LOG_FN_EXIT();
}

/**
 * @brief Set the bit flags for which hooks we're going to use
 *
 * This plugin uses all the hooks so set all the flags
 */
unsigned NetworkingPlugin::hookHints() const
{
    return (
        IDobbyRdkPlugin::HintFlags::PostInstallationFlag |
        IDobbyRdkPlugin::HintFlags::CreateRuntimeFlag |
        IDobbyRdkPlugin::HintFlags::PostHaltFlag
    );
}

// Begin Hook Methods

/**
 * @brief Dobby Hook - run in host namespace *once* when container bundle is downloaded
 */
bool NetworkingPlugin::postInstallation()
{
    AI_LOG_FN_ENTRY();

    if (!mValid)
    {
        AI_LOG_ERROR_EXIT("invalid config file");
        return false;
    }

    // if the network type is not 'open', enable network namespacing in OCI config
    if (mNetworkType != NetworkType::Open)
    {
        // add /etc/resolv.conf mount if not using dnsmasq. If dnsmasq is enabled,
        // a new /etc/resolv.conf is created rather than mounting the host's
        if (!mPluginData->dnsmasq)
        {
            AI_LOG_INFO("Adding resolv.conf mount");
            NetworkSetup::addResolvMount(mUtils, mContainerConfig);
        }

        // add network namespacing to the OCI config
        NetworkSetup::addNetworkNamespace(mContainerConfig);
    }

    AI_LOG_FN_EXIT();
    return true;
}


/**
 * @brief OCI Hook - Run in host namespace
 */
bool NetworkingPlugin::createRuntime()
{
    AI_LOG_FN_ENTRY();

    if (!mValid)
    {
        AI_LOG_ERROR_EXIT("invalid config file");
        return false;
    }

    // nothing to do for containers configured for an open network
    if (mNetworkType == NetworkType::Open)
    {
        AI_LOG_FN_EXIT();
        return true;
    }

    // get available external interfaces
    const std::vector<std::string> extIfaces = GetAvailableExternalInterfaces();
    if (extIfaces.empty())
    {
        AI_LOG_ERROR_EXIT("No network interfaces available");
        return false;
    }

    // check if another container has already initialised the bridge device for us
    std::shared_ptr<Netlink> netlink = std::make_shared<Netlink>();
    bool bridgeExists = netlink->ifaceExists(std::string(BRIDGE_NAME));

    if (!bridgeExists)
    {
        AI_LOG_DEBUG("Dobby network bridge not found, setting it up");

        // setup the bridge device
        if (!NetworkSetup::setupBridgeDevice(mUtils, mNetfilter, extIfaces))
        {
            AI_LOG_ERROR_EXIT("failed to setup Dobby bridge device");
            return false;
        }
    }

    // setup veth, ip address and iptables rules for container
    if (!NetworkSetup::setupVeth(mUtils, mNetfilter, mDobbyProxy, mHelper,
                                 mRootfsPath, mUtils->getContainerId(), mNetworkType))
    {
        AI_LOG_ERROR_EXIT("failed to setup virtual ethernet device");
        return false;
    }

    // setup dnsmasq rules if enabled
    if (mNetworkType != NetworkType::None && mPluginData->dnsmasq)
    {
        if (!DnsmasqSetup::set(mUtils, mNetfilter, mHelper, mRootfsPath,
                               mUtils->getContainerId(), mNetworkType))
        {
            AI_LOG_ERROR_EXIT("failed to setup container for dnsmasq use");
            return false;
        }
    }

    // add port forwards if any have been configured
    if (mPluginData->port_forwarding != nullptr)
    {
        if (!PortForwarding::addPortForwards(mNetfilter, mHelper, mUtils->getContainerId(), mPluginData->port_forwarding))
        {
            AI_LOG_ERROR_EXIT("failed to add port forwards");
            return false;
        }

        // Add localhost masquerade if enabled (run in container network namespace)
        if (mPluginData->port_forwarding->localhost_masquerade_present && mPluginData->port_forwarding->localhost_masquerade)
        {
            // Ideally this would be done in the createContainer hook, but that fails
            // on some platforms with permissions issues (works fine on VM...)
            if (!mUtils->callInNamespace(mUtils->getContainerPid(), CLONE_NEWNET,
                                         &PortForwarding::addLocalhostMasquerading,
                                         mHelper,
                                         mUtils,
                                         mPluginData->port_forwarding))
            {
                AI_LOG_ERROR_EXIT("Failed to add localhost masquerade iptables rules inside container");
                return false;
            }
        }
    }

    // enable multicast forwarding
    if (mPluginData->multicast_forwarding != nullptr)
    {
        if (!MulticastForwarder::set(mNetfilter, mPluginData, mHelper->vethName(), mUtils->getContainerId(), extIfaces))
        {
            AI_LOG_ERROR_EXIT("failed to add multicast forwards");
            return false;
        }
    }

    // apply iptables changes
    if (!mNetfilter->applyRules(AF_INET) || !mNetfilter->applyRules(AF_INET6))
    {
        AI_LOG_ERROR_EXIT("failed to apply iptables rules");
        return false;
    }

    AI_LOG_FN_EXIT();
    return true;
}

/**
 * @brief Dobby Hook - Run in host namespace when container terminates
 */
bool NetworkingPlugin::postHalt()
{
    AI_LOG_FN_ENTRY();

    bool success = true;

    if (!mValid)
    {
        AI_LOG_ERROR_EXIT("invalid config file");
        return false;
    }

    // nothing to do for containers configured for an open network
    if (mNetworkType == NetworkType::Open)
    {
        AI_LOG_FN_EXIT();
        return true;
    }

    ContainerNetworkInfo networkInfo;
    if (!mUtils->getContainerNetworkInfo(networkInfo))
    {
        AI_LOG_WARN("Failed to get container network info");
        success = false;
    }
    else
    {
        mHelper->storeContainerInterface(NetworkingHelper::StringToIpAddress(networkInfo.ipAddress), networkInfo.vethName);

        // delete the veth pair for the container
        if (!NetworkSetup::removeVethPair(mNetfilter, mHelper, networkInfo.vethName, mNetworkType, mUtils->getContainerId()))
        {
            AI_LOG_WARN("failed to remove veth pair %s", networkInfo.vethName.c_str());
            success = false;
        }
    }

    // remove the address file from the host
    const std::string addressFilePath = ADDRESS_FILE_DIR + mUtils->getContainerId();
    if (unlink(addressFilePath.c_str()) == -1)
    {
        AI_LOG_WARN("failed to remove address file for container %s at %s",
                    mUtils->getContainerId().c_str(), addressFilePath.c_str());
        success = false;
    }

    // get external interfaces from daemon
    const std::vector<std::string> extIfaces = GetExternalInterfacesFromSettings();
    if (extIfaces.empty())
    {
        AI_LOG_WARN("couldn't find external network interfaces in settings,"
                    "unable to remove bridge device");
        success = false;
    }
    else
    {
        // if there are no containers using the bridge device left, remove bridge device
        std::shared_ptr<Netlink> netlink = std::make_shared<Netlink>();
        auto bridgeConnections = netlink->getAttachedIfaces(BRIDGE_NAME);

        if (bridgeConnections.size() == 0 || (bridgeConnections.size() == 1 && strcmp(bridgeConnections.front().name, "dobby_tap0") == 0))
        {
            if (!NetworkSetup::removeBridgeDevice(mNetfilter, extIfaces))
            {
                success = false;
            }
        }
    }

    // if dnsmasq iptables rules were set up for container, "uninstall" them
    if (mNetworkType != NetworkType::None && mPluginData->dnsmasq)
    {
        if (!DnsmasqSetup::removeRules(mNetfilter, mHelper, mUtils->getContainerId()))
        {
            success = false;
        }
    }

    // remove port forwards if any have been configured
    // no need to remove the localhost masquerade rules as these were only
    // applied inside the container namespace
    if (mPluginData->port_forwarding != nullptr)
    {
        if (!PortForwarding::removePortForwards(mNetfilter, mHelper, mUtils->getContainerId(), mPluginData->port_forwarding))
        {
            success = false;
        }
    }

    // remove multicast forwarding rules if configured
    if (mPluginData->multicast_forwarding != nullptr)
    {
        if (!MulticastForwarder::removeRules(mNetfilter, mPluginData, mHelper->vethName(), mUtils->getContainerId(), extIfaces))
        {
            AI_LOG_ERROR_EXIT("failed to remove multicast forwards");
            return false;
        }
    }

    // apply iptables changes
    if (!mNetfilter->applyRules(AF_INET) || !mNetfilter->applyRules(AF_INET6))
    {
        AI_LOG_ERROR_EXIT("failed to apply iptables rules");
        return false;
    }

    AI_LOG_FN_EXIT();
    return success;
}

// End hook methods

/**
 * @brief Should return the names of the plugins this plugin depends on.
 *
 * This can be used to determine the order in which the plugins should be
 * processed when running hooks.
 *
 * @return Names of the plugins this plugin depends on.
 */
std::vector<std::string> NetworkingPlugin::getDependencies() const
{
    std::vector<std::string> dependencies;
    const rt_defs_plugins_networking* pluginConfig = mContainerConfig->rdk_plugins->networking;

    for (size_t i = 0; i < pluginConfig->depends_on_len; i++)
    {
        dependencies.push_back(pluginConfig->depends_on[i]);
    }

    return dependencies;
}

// Begin private methods

std::vector<std::string> NetworkingPlugin::GetAvailableExternalInterfaces() const
{
    std::vector<std::string> externalIfaces = GetExternalInterfacesFromSettings();

    // Look in the /sys/class/net for available interfaces
    struct dirent *dir;
    DIR *d = opendir("/sys/class/net");
    if (!d)
    {
        AI_LOG_SYS_ERROR(errno, "Could not check for available interfaces");
        return std::vector<std::string>();
    }

    std::vector<std::string> availableIfaces = {};
    while ((dir = readdir(d)) != nullptr)
    {
        if (dir->d_name[0] != '.')
        {
            availableIfaces.emplace_back(dir->d_name);
        }
    }
    closedir(d);

    // We know what interfaces we want, and what we've got. See if we're missing
    // any
    auto it = externalIfaces.cbegin();
    while (it != externalIfaces.cend())
    {
        if (std::find(availableIfaces.begin(), availableIfaces.end(), it->c_str()) == availableIfaces.end())
        {
            AI_LOG_WARN("Interface '%s' from settings file not available", it->c_str());
            externalIfaces.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // If no interfaces are available, something is very wrong
    if (externalIfaces.size() == 0)
    {
        AI_LOG_ERROR("None of the external interfaces defined in the settings file are available");
    }

    return externalIfaces;
}

std::vector<std::string> NetworkingPlugin::GetExternalInterfacesFromSettings() const
{
    // We don't have jsoncpp here, so use yajl from libocispec
    std::vector<std::string> ifacesFromSettings = {};

    std::string settingsFile = mUtils->readTextFile("/etc/dobby.json");

    if (settingsFile.empty())
    {
        AI_LOG_ERROR("Could not read file @ '/etc/dobby.json'");
        return {};
    }

    yajl_val tree;
    parser_error err = nullptr;
    char errbuf[1024];

    // Parse the settings file
    tree = yajl_tree_parse (settingsFile.c_str(), errbuf, sizeof (errbuf));
    if (!tree || err)
    {
        AI_LOG_ERROR_EXIT("Failed to parse Dobby settings file, err '%s'", err);
        return {};
    }

    // Read the external interfaces array
    const char* path[] = {"network", "externalInterfaces", (const char *) 0};
    yajl_val extInterfaces = yajl_tree_get(tree, path, yajl_t_array);
    if (extInterfaces && YAJL_GET_ARRAY (extInterfaces) && YAJL_GET_ARRAY (extInterfaces)->len > 0)
    {
        size_t i;
        size_t len = YAJL_GET_ARRAY (extInterfaces)->len;
        yajl_val *values = YAJL_GET_ARRAY (extInterfaces)->values;

        for (i = 0; i < len; i++)
        {
            ifacesFromSettings.push_back(YAJL_GET_STRING(values[i]));
        }
    }

    yajl_tree_free(tree);

    return ifacesFromSettings;
}