using System.Reflection;
using System.Runtime.ExceptionServices;
using Overseer.Api;

namespace Overseer.Host.Plugins;

/// <summary>Turns a <see cref="CommandAttribute"/> method into a router handler.</summary>
internal static class CommandInvoker
{
    public static Func<CommandContext, CancellationToken, Task<string?>> Create(object target, MethodInfo method)
    {
        var parameters = method.GetParameters();
        var takesContext = parameters.Length == 1 && parameters[0].ParameterType == typeof(CommandContext);
        if (parameters.Length > 1 || (parameters.Length == 1 && !takesContext))
        {
            throw new InvalidOperationException(
                $"{method.DeclaringType?.Name}.{method.Name}: command methods take no parameters or a single CommandContext.");
        }

        var returnType = method.ReturnType;
        if (returnType != typeof(void) && returnType != typeof(string) &&
            returnType != typeof(Task) && returnType != typeof(Task<string>))
        {
            throw new InvalidOperationException(
                $"{method.DeclaringType?.Name}.{method.Name}: command methods must return void, string, Task or Task<string>.");
        }

        return async (context, _) =>
        {
            object? result;
            try
            {
                result = method.Invoke(target, takesContext ? [context] : []);
            }
            catch (TargetInvocationException ex) when (ex.InnerException is not null)
            {
                ExceptionDispatchInfo.Capture(ex.InnerException).Throw();
                throw;
            }

            switch (result)
            {
                case Task<string> textTask:
                    return await textTask.ConfigureAwait(false);
                case Task task:
                    await task.ConfigureAwait(false);
                    return null;
                case string text:
                    return text;
                default:
                    return null;
            }
        };
    }
}
